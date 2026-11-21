#include "csv.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

template<int ISA> uint64_t scan(const std::vector<std::string>& files) {
    uint64_t sum=0;
    for(const auto& f: files) {
        std::string_view input=f;
        const auto header=input.find('\n');
        if(header==input.npos)continue;
        input.remove_prefix(header+1);
        while(!input.empty()) {
            auto lf=input.find('\n');
            auto row=input.substr(0,lf);
            std::array<std::string_view,7> fields;
            bool good;
            if(row.size()<32 || row.size()>128)good=split_fields_generic(row,fields);
            else if constexpr(ISA==0)good=csv_isa::avx2(row,fields);
            else if constexpr(ISA==1)good=csv_isa::avx512(row,fields);
            else good=csv_isa::vbmi2(row,fields);
            if(good)for(auto field:fields)sum+=field.size()+(field.empty()?0:uint8_t(field.front()));
            if(lf==input.npos)break;
            input.remove_prefix(lf+1);
        }
    }
    return sum;
}
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Pass path to the events directory\n";return 1;}
    std::vector<std::string> files;
    for(const auto& item:std::filesystem::directory_iterator(argv[1])) {
        if(item.path().extension()!=".csv")continue;
        std::ifstream source(item.path(),std::ios::binary);
        files.emplace_back(std::istreambuf_iterator<char>{source},std::istreambuf_iterator<char>{});
    }
    bool wide=__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
    bool vbmi=wide && __builtin_cpu_supports("avx512vbmi2");
    std::cout<<"AVX512BW="<<wide<<" AVX512VBMI2="<<vbmi<<"\n";
    const uint64_t expected=scan<0>(files);
    std::vector<double> times[3];
    for(int rep=0;rep<6;rep++)for(int j=0;j<3;j++){
        int isa=(j+rep)%3;
        if((isa==1&&!wide)||(isa==2&&!vbmi))continue;
        auto start=std::clock();
        auto got=isa==0?scan<0>(files):isa==1?scan<1>(files):scan<2>(files);
        double elapsed=double(std::clock()-start)/CLOCKS_PER_SEC;
        if(got!=expected){std::cerr<<"Mismatch "<<isa<<"\n";return 2;}
        times[isa].push_back(elapsed);
    }
    for(int i=0;i<3;i++)if(!times[i].empty()){
        auto& t=times[i];std::sort(t.begin(),t.end());
        std::cout<<(i==0?"AVX2":i==1?"AVX512BW":"AVX512VBMI2")<<" median CPU seconds "<<(t[2]+t[3])/2<<"\n";
    }
}
