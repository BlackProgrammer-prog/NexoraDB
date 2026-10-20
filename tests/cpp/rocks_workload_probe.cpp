#include "TestTempDir.h"
#include "core/DocEngine.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>
using namespace nexora::core;
using namespace nexora::query;
using Clock=std::chrono::steady_clock;
namespace {
void check(const DBResult& result) { if (!result.success) throw std::runtime_error(result.error_msg); }
double us(Clock::duration duration) { return std::chrono::duration<double,std::micro>(duration).count(); }
double percentile(std::vector<double>& values,double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end());
    return values[size_t(std::ceil(fraction*values.size()))-1];
}
void run(const std::string& profile,char workload,int repeat,size_t records,size_t operations) {
    TestTempDir temp("rocks_workload"); RocksSettings settings;
    settings.memory_budget_bytes=32*1024*1024;
    if (profile=="rate8m") settings.rate_limit_bytes_per_second=8*1024*1024;
    else if (profile=="block16k") settings.block_size=16384;
    else if (profile=="static") settings.dynamic_level_bytes=false;
    else if (profile=="jobs1") settings.max_background_jobs=1;
    else if (profile=="buffer1m") settings.write_buffer_bytes=1024*1024;
    else if (profile=="sync0") settings.bytes_per_sync=settings.wal_bytes_per_sync=0;
    else if (profile=="sync4m") settings.bytes_per_sync=settings.wal_bytes_per_sync=4*1024*1024;
    else if (profile=="snappy") settings.hot_compression=HotCompression::Snappy;
    else if (profile=="nozstd") settings.cold_zstd=false;
    else if (profile=="stats_off") settings.statistics_enabled=false;
    else if (profile=="durable") settings.durability=DocumentDurability::Durable;
    else if (profile!="baseline") throw std::invalid_argument("unknown profile");
    DocEngine engine(temp.path().string(),{},settings); check(engine.CreateCollection("bench"));
    std::mt19937_64 generator(1729);
    auto payload=[&] { std::string value(100,' '); for (auto& ch:value) ch="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[generator()%62]; return value; };
    const auto load_start=Clock::now();
    for (size_t start=0;start<records;start+=500) {
        std::vector<std::string> batch;
        for (size_t i=start;i<std::min(records,start+500);++i) {
            nlohmann::json document; document["_id"]=std::to_string(i);
            for (int field=0;field<10;++field) document["field"+std::to_string(field)]=payload();
            batch.push_back(document.dump());
        }
        check(engine.InsertMany("bench",batch));
    }
    check(engine.FlushStorage());
    const double load_ms=us(Clock::now()-load_start)/1000;
    // Zipf-like rank distribution, exponent .99, deterministic permutation of
    // ranks. Not YCSB's Java ScrambledZipfianGenerator or a certified YCSB run.
    std::vector<double> cumulative(records); double sum=0;
    for (size_t i=0;i<records;++i) cumulative[i]=(sum+=1/std::pow(double(i+1),.99));
    std::vector<size_t> permutation(records);
    for(size_t i=0;i<records;++i) permutation[i]=i;
    std::shuffle(permutation.begin(),permutation.end(),generator);
    for(size_t i=0;i<std::min<size_t>(records,2000);++i) check(engine.FindById("bench",std::to_string(permutation[i])));
    const auto before=engine.GetRocksMetrics();
    std::atomic<bool> start=false,stop=false;
    std::atomic<size_t> ready=0,errors=0;
    uint64_t peak_debt=0,peak_l0=0,stopped_samples=0,slowed_samples=0;
    std::thread monitor([&] {
        while(!start.load()) std::this_thread::yield();
        while(!stop.load()) {
            const auto metrics=engine.GetRocksMetrics();
            peak_debt=std::max(peak_debt,metrics.pending_compaction_bytes);
            peak_l0=std::max(peak_l0,metrics.l0_files);
            stopped_samples+=metrics.write_stopped!=0;
            slowed_samples+=metrics.delayed_write_rate!=0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    constexpr size_t threads=4;
    std::vector<double> reads[threads],writes[threads]; std::vector<std::thread> workers;
    for(size_t worker=0;worker<threads;++worker) workers.emplace_back([&,worker] {
        std::mt19937_64 rng(991+worker); std::uniform_real_distribution<double> rank(0,sum);
        reads[worker].reserve(operations/threads); writes[worker].reserve(operations/threads);
        ++ready; while(!start.load()) std::this_thread::yield();
        for(size_t op=worker;op<operations;op+=threads) {
            auto position=size_t(std::lower_bound(cumulative.begin(),cumulative.end(),rank(rng))-cumulative.begin());
            const auto key=std::to_string(permutation[std::min(position,records-1)]);
            // Exact operation proportions for operation counts divisible by 100.
            const bool write=workload=='A' ? op%100<50 : workload=='B' && op%100<5;
            UpdateSpec update;
            if (write) {
                std::string value(100,' ');
                for(auto& ch:value) ch="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[rng()%62];
                update.Set("field"+std::to_string(rng()%10),value,UpdateValueType::String);
            }
            const auto begin=Clock::now();
            const auto result=write ? engine.UpdateById("bench",key,update) : engine.FindById("bench",key);
            const auto elapsed=us(Clock::now()-begin);
            if(!result.success) ++errors;
            (write ? writes[worker] : reads[worker]).push_back(elapsed);
        }
    });
    while(ready.load()!=threads) std::this_thread::yield();
    const auto begin=Clock::now(); start=true;
    for(auto& worker:workers) worker.join();
    const double seconds=us(Clock::now()-begin)/1e6; stop=true; monitor.join();
    const auto after=engine.GetRocksMetrics();
    std::vector<double> read,write;
    for(size_t i=0;i<threads;++i) { read.insert(read.end(),reads[i].begin(),reads[i].end()); write.insert(write.end(),writes[i].begin(),writes[i].end()); }
    nlohmann::json output={{"profile",profile},{"workload",std::string(1,workload)},{"repeat",repeat},
        {"records",records},{"operations",operations},{"threads",threads},{"load_ms",load_ms},{"ops_s",operations/seconds},
        {"reads",read.size()},{"writes",write.size()},{"errors",errors.load()},
        {"read_p50_us",percentile(read,.50)},{"read_p95_us",percentile(read,.95)},{"read_p99_us",percentile(read,.99)},
        {"write_p50_us",percentile(write,.50)},{"write_p95_us",percentile(write,.95)},{"write_p99_us",percentile(write,.99)},
        {"read_max_us",read.empty()?0:read.back()},{"write_max_us",write.empty()?0:write.back()},
        {"stall_us",after.stall_micros-before.stall_micros},{"peak_debt",peak_debt},{"peak_l0",peak_l0},
        {"stopped_samples",stopped_samples},{"slowed_samples",slowed_samples},
        {"logical_write_bytes",after.logical_write_bytes-before.logical_write_bytes},
        {"flush_bytes",after.flush_write_bytes-before.flush_write_bytes},
        {"compact_bytes",after.compaction_write_bytes-before.compaction_write_bytes},
        {"cache_hit_rate",after.cache_hit_rate},{"bloom_useful_rate",after.bloom_useful_rate}};
    std::cout<<output.dump()<<std::endl;
    if(errors.load()) throw std::runtime_error("workload operation failure");
}
}
int main(int argc,char** argv) {
    try {
        const std::string selected=argc>1?argv[1]:"baseline";
        const size_t records=argc>2?std::stoull(argv[2]):50000,operations=argc>3?std::stoull(argv[3]):50000;
        const int repeats=argc>4?std::stoi(argv[4]):3;
        if(records<100 || operations<100 || repeats<1) throw std::invalid_argument("minimum 100 records/operations and one repeat");
        for(int repeat=0;repeat<repeats;++repeat) {
            std::vector<std::string> profiles={"baseline","rate8m","block16k","static","jobs1","buffer1m","sync0","sync4m","snappy","nozstd","stats_off","durable"};
            // Deterministic rotation avoids always running baseline first.
            std::rotate(profiles.begin(),profiles.begin()+repeat%profiles.size(),profiles.end());
            for(const auto& profile:profiles) if(selected=="all" || selected==profile) {
                run(profile,'A',repeat,records,operations);
                if(profile=="baseline") { run(profile,'B',repeat,records,operations); run(profile,'C',repeat,records,operations); }
            }
        }
    } catch(const std::exception& error) { std::cerr<<error.what()<<std::endl; return 1; }
}
