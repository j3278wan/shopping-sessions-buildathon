#include "csv.hpp"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

template<size_t N>
void check(std::string_view row, bool wide) {
    std::array<std::string_view, N> reference, actual;
    const auto valid = split_fields_generic(row, reference);
    auto compare = [&](bool result, const char* variant) {
        if (result != valid || (result && actual != reference)) {
            std::cerr << "Mismatch in " << variant << " at length " << row.size() << "\n";
            std::exit(2);
        }
    };
    compare(split_fields(row, actual), "automatic dispatch");
    if (row.size() >= 32 && row.size() <= 128) {
        compare(csv_isa::avx2(row, actual), "AVX2");
        if (wide) compare(csv_isa::avx512(row, actual), "AVX512BW");
        if (wide && __builtin_cpu_supports("avx512vbmi2"))
            compare(csv_isa::vbmi2(row, actual), "AVX512VBMI2");
        csv_isa::Masks lo{}, hi{};
        uint64_t cuts=0;
        unsigned count=0;
        for(size_t i=0;i<row.size();++i){
            auto &m=i<64?lo:hi;
            m.commas |= uint64_t(row[i]==',')<<(i&63);
            m.quotes |= uint64_t(row[i]=='"')<<(i&63);
            if(row[i]==',') {if(count<N-1)cuts|=uint64_t(i)<<(8*count);++count;}
        }
        uint32_t starts;
        bool accepted=count==N-1 && csv_isa::quote_edges<N>(row.size(),lo,hi,__builtin_popcountll(lo.commas),starts)
                     && csv_isa::packed_fields(row,actual,cuts,starts);
        compare(accepted,"packed-structural decoder");
    }
}

int main() {
    bool wide = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
    std::cout << "Default split_fields ISA for length 32..128: "
              << (wide && __builtin_cpu_supports("avx512vbmi2") ? "AVX512VBMI2" : wide ? "AVX512BW" : "AVX2") << "\n";
    const size_t page = sysconf(_SC_PAGESIZE);
    auto* data = static_cast<char*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (data == MAP_FAILED || mprotect(data + page, page, PROT_NONE)) return 3;
    std::mt19937 rng(2489);
    for (int i = 0; i != 150000; ++i) {
        std::string row;
        for (unsigned j = 0, fields = (rng() & 1) ? 7 : 4; j < fields; ++j) {
            if (j) row += ',';
            bool quoted = rng() & 1;
            if (quoted) row += '"';
            for (unsigned k = 0, n = rng() % 30; k < n; ++k) {
                unsigned char c = rng() & 255;
                row += (c == ',' || c == '"' || c == '\n') ? 'x' : char(c);
            }
            if (quoted) row += '"';
        }
        if (i & 1) row[rng() % row.size()] = "\",\0"[rng() % 3];
        char* start = data + page - row.size();
        std::memcpy(start, row.data(), row.size());
        std::string_view guarded(start, row.size());
        check<4>(guarded, wide);
        check<7>(guarded, wide);
    }
    if (munmap(data, 2 * page)) return 4;
    std::cout << "Guard-page differential passed: generic, dispatch, AVX2"
              << (wide ? ", and AVX512BW" : "") << "\n";
}
