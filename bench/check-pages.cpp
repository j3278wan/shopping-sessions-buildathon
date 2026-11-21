#include "records.hpp"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

template<size_t N> void check(std::string_view input) {
    using Row=std::array<std::string_view,N>;
    std::vector<Row> expect,actual;
    auto tail=input;
    while(!tail.empty()){
        auto lf=tail.find('\n');Row fields;
        if(split_fields_generic(tail.substr(0,lf),fields))expect.push_back(fields);
        if(lf==tail.npos)break;
        tail.remove_prefix(lf+1);
    }
    strict_records::rows<N>(input,[&](const Row& row){actual.push_back(row);});
    if(expect!=actual){std::cerr<<"Mismatch in "<<N<<" fields, "<<input.size()<<" bytes\n";std::exit(2);}
}
int main(){
    size_t page=sysconf(_SC_PAGESIZE),limit=32*page;
    auto data=static_cast<char*>(mmap(nullptr,limit+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(data==MAP_FAILED||mprotect(data+limit,page,PROT_NONE))return 3;
    std::mt19937 gen(42);
    for(size_t i=0;i<2500;i++){
        std::string blob;
        auto rows=1+gen()%200;
        for(unsigned r=0;r<rows;r++){
            auto n=(gen()&1)?4:7;
            for(int j=0;j<n;j++){
                if(j)blob+=',';
                bool quoted=gen()&1;
                if(quoted)blob+='"';
                unsigned size=gen()%40;
                if(r==0 && j==0 && i%5==0)size=4090+gen()%10000;
                for(unsigned k=0;k<size;k++){
                    char c=char(gen()&255);
                    if(c==','||c=='"'||c=='\n')c='x';
                    blob+=c;
                }
                if(quoted)blob+='"';
            }
            if(i%7==0 && !blob.empty())blob[gen()%blob.size()]="\n\",\0"[gen()%4];
            if(r+1!=rows || (i&1))blob+='\n';
        }
        if(blob.size()>limit)blob.resize(limit);
        char* guarded=data+limit-blob.size();
        std::memcpy(guarded,blob.data(),blob.size());
        auto view=std::string_view(guarded,blob.size());
        check<7>(view);check<4>(view);
    }
    munmap(data,limit+page);
    std::cout<<"2500 multi-page CSV blocks and long/unterminated rows matched an independent splitter\n";
}
