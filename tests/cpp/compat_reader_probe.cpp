// Link against an archived reader's core/query/RocksDB libraries, NOT current
// nexora_core. Compile with the archived reader's HEADERS as well as libraries;
// current DocEngine has new telemetry members and is not layout-compatible.
#include "core/DocEngine.h"
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2) return 64;
    try { nexora::core::DocEngine reader(argv[1]); std::cout<<"opened\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; }
}
