#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// The input dialect forbids embedded commas, quotes and newlines. Collect the
// commas in parallel and count all quotes. A valid quoted field accounts for
// exactly its opening and closing quotes, so no second scan of its contents is needed.
template <size_t N> bool split_fields_generic(std::string_view line, std::array<std::string_view, N> &fields) {
    size_t cuts[N];
    size_t count = 0, quotes = 0, pos = 0;
    const char *data = line.data();
#if defined(__AVX2__)
    const auto comma = _mm256_set1_epi8(','), quote = _mm256_set1_epi8('"');
    for (; pos + 32 <= line.size(); pos += 32) {
        auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + pos));
        uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, comma));
        quotes += __builtin_popcount(
            static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, quote))));
        while (mask) {
            if (count == N - 1)
                return false;
            cuts[count++] = pos + __builtin_ctz(mask);
            mask &= mask - 1;
        }
    }
#elif defined(__aarch64__) && defined(__ARM_NEON)
    const auto comma = vdupq_n_u8(','), quote = vdupq_n_u8('"');
    for (; pos + 16 <= line.size(); pos += 16) {
        auto bytes = vld1q_u8(reinterpret_cast<const uint8_t *>(data + pos));
        auto matches = vceqq_u8(bytes, comma);
        // Narrow each pair into one byte, leaving four bits per input position.
        uint64_t mask =
            vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(matches), 4)), 0) &
            0x1111111111111111ULL;
        quotes += vaddvq_u8(vshrq_n_u8(vceqq_u8(bytes, quote), 7));
        while (mask) {
            if (count == N - 1)
                return false;
            cuts[count++] = pos + __builtin_ctzll(mask) / 4;
            mask &= mask - 1;
        }
    }
#endif
    for (; pos < line.size(); ++pos) {
        if (data[pos] == ',') {
            if (count == N - 1)
                return false;
            cuts[count++] = pos;
        }
        quotes += data[pos] == '"';
    }
    if (count != N - 1)
        return false;
    cuts[count] = line.size();
    size_t start = 0, accounted = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t length = cuts[i] - start;
        if (length && data[start] == '"') {
            if (length < 2 || data[cuts[i] - 1] != '"')
                return false;
            fields[i] = line.substr(start + 1, length - 2);
            accounted += 2;
        } else {
            fields[i] = line.substr(start, length);
        }
        start = cuts[i] + 1;
    }
    return accounted == quotes;
}

#if defined(__x86_64__) && defined(__AVX2__) && defined(__BMI2__)
namespace csv_isa {
struct Masks {uint64_t commas,quotes;};
inline Masks scan32(const char* data) {
    auto v=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    return {uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8(',')))),
            uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8('"'))))};
}
inline Masks range32to64(const char* p,unsigned n) {
    auto a=scan32(p);
    if(n==32)return a;
    auto b=scan32(p+n-32);unsigned shift=n-32;
    return {a.commas|(b.commas<<shift),a.quotes|(b.quotes<<shift)};
}
template<size_t N>
inline bool decode(std::string_view line,std::array<std::string_view,N>& fields,Masks low,Masks high) {
    unsigned n=static_cast<unsigned>(line.size());
    unsigned low_count=__builtin_popcountll(low.commas);
    if(low_count+__builtin_popcountll(high.commas)!=N-1)return false;
    const uint32_t starts=(low.quotes&1)
        | (uint32_t(_pext_u64((low.quotes>>1)|(high.quotes<<63),low.commas))<<1)
        | (uint32_t(_pext_u64(high.quotes>>1,high.commas))<<(low_count+1));
    const uint32_t final_quote=((n<=64?low.quotes:high.quotes)>>((n-1)&63))&1;
    const uint32_t finishes=uint32_t(_pext_u64(low.quotes<<1,low.commas))
        | (uint32_t(_pext_u64((high.quotes<<1)|(low.quotes>>63),high.commas))<<low_count)
        | (final_quote << (N-1));
    if(starts!=finishes || __builtin_popcountll(low.quotes)+__builtin_popcountll(high.quotes)!=2*__builtin_popcount(starts))return false;
    uint8_t cuts[N];unsigned count=0;
    while(low.commas){cuts[count++]=__builtin_ctzll(low.commas);low.commas&=low.commas-1;}
    while(high.commas){cuts[count++]=64+__builtin_ctzll(high.commas);high.commas&=high.commas-1;}
    cuts[N-1]=static_cast<uint8_t>(n);
    unsigned begin=0;
    for(unsigned i=0;i<N;++i){
        unsigned width=cuts[i]-begin,q=(starts>>i)&1;
        if(width<2*q)return false;
        fields[i]={line.data()+begin+q,width-2*q};begin=cuts[i]+1;
    }
    return true;
}
template<size_t N> bool avx2(std::string_view line,std::array<std::string_view,N>& f) {
    unsigned n=static_cast<unsigned>(line.size());
    if(n<=64)return decode(line,f,range32to64(line.data(),n),{});
    Masks a=range32to64(line.data(),64),b;
    if(n<96) {
        unsigned skip=96-n;auto last=scan32(line.data()+n-32);
        b={last.commas>>skip,last.quotes>>skip};
    } else b=range32to64(line.data()+64,n-64);
    return decode(line,f,a,b);
}
__attribute__((target("avx512f,avx512bw"))) inline Masks scan64(const char* d,unsigned n) {
    auto valid=~uint64_t{0}>>(64-n);
    auto bytes=_mm512_maskz_loadu_epi8(valid,d);
    return {_mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8(',')),
            _mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8('"'))};
}
template<size_t N> __attribute__((target("avx512f,avx512bw"))) bool avx512(std::string_view line,std::array<std::string_view,N>& f) {
    unsigned n=static_cast<unsigned>(line.size());
    if(n<=64)return decode(line,f,scan64(line.data(),n),{});
    return decode(line,f,scan64(line.data(),64),scan64(line.data()+64,n-64));
}
}
#endif
template<size_t N>
bool split_fields(std::string_view line,std::array<std::string_view,N>& f) {
#if defined(__x86_64__) && defined(__AVX2__) && defined(__BMI2__)
    if constexpr(N==7 || N==4) {
        if(line.size()>=32 && line.size()<=128) {
            static const bool wide=__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
            if(wide)return csv_isa::avx512(line,f);
            return csv_isa::avx2(line,f);
        }
    }
#endif
    return split_fields_generic(line,f);
}
