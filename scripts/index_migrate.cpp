#include "core/DocEngine.h"
#include <iostream>
#include <string>
#include <stdexcept>

int main(int argc,char** argv) {
    std::string source,destination;bool apply=false;uint64_t pause_after=0;
    try {
        for(int i=1;i<argc;++i) {
            const std::string option=argv[i];
            if(option=="--apply") apply=true;
            else if(option=="--dry-run") apply=false;
            else if((option=="--source" || option=="--destination" || option=="--test-pause-after") && i+1<argc) {
                const std::string value=argv[++i];
                if(option=="--source") source=value;
                else if(option=="--destination") destination=value;
                else pause_after=std::stoull(value);
            } else throw std::runtime_error("Usage: nexora_index_migrate --source CLOSED_DB [--dry-run | --apply --destination NEW_DB]");
        }
        if(source.empty() || (apply && destination.empty())) throw std::runtime_error("Source and fresh apply destination required");
        const auto result=nexora::core::DocEngine::MigrateIndexFormatCopy(source,destination,!apply,[&](uint64_t records) {
            if(records%1000==0 || records==pause_after) std::cerr<<"{\"event\":\"migration-progress\",\"records\":"<<records<<"}"<<std::endl;
            if(pause_after && records==pause_after) {
                std::cerr<<"{\"event\":\"test-paused-before-publish\"}"<<std::endl;
                std::string resume;if(!std::getline(std::cin,resume)) throw std::runtime_error("test pause interrupted");
            }
        });
        if(!result.success) {std::cerr<<result.error_msg<<std::endl;return 1;}
        std::cout<<result.data<<std::endl;return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;return 1;}
}
