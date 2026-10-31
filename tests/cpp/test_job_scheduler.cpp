#include "TestTempDir.h"
#include "graph/GraphManager.h"
#include <gtest/gtest.h>
#include <future>
#include <thread>
using namespace nexora::graph;
using namespace std::chrono_literals;
namespace {
auto token() { return std::make_shared<std::atomic<bool>>(false); }
auto ok() { return algorithms::AlgoResult{true,"","{}",0}; }
}
TEST(JobScheduler, PriorityFifoBoundedQueueAndMetrics) {
    JobScheduler pool({1,3,10}); std::promise<void> started,release;
    auto signal=release.get_future().share();
    auto active=pool.submit([&] { started.set_value(); signal.wait(); return ok(); },token(),0,1,0ms);
    started.get_future().wait(); std::vector<int> order;
    auto low=pool.submit([&] { order.push_back(1); return ok(); },token(),0,1,0ms);
    auto high=pool.submit([&] { order.push_back(2); return ok(); },token(),5,1,0ms);
    auto equal=pool.submit([&] { order.push_back(3); return ok(); },token(),5,1,0ms);
    const auto rejected=pool.submit([] { return ok(); },token(),0,1,0ms).get();
    EXPECT_FALSE(rejected.success); EXPECT_NE(rejected.error_msg.find("overloaded"),std::string::npos);
    const auto stats=pool.stats(); EXPECT_EQ(stats.running,1U); EXPECT_EQ(stats.queued,3U);
    release.set_value(); EXPECT_TRUE(active.get().success);
    const auto result=low.get(); EXPECT_TRUE(result.success); EXPECT_GE(result.queue_ms,0);
    EXPECT_GE(result.execution_ms,0); EXPECT_EQ(result.reserved_bytes,1U);
    EXPECT_TRUE(high.get().success); EXPECT_TRUE(equal.get().success);
    EXPECT_EQ(order,(std::vector<int>{2,3,1})); pool.shutdown();
    EXPECT_EQ(pool.stats().reserved_bytes,0U);
}
TEST(JobScheduler, ResourceAdmissionTimedBackpressureAndNestedSubmit) {
    JobScheduler pool({1,8,2}); std::promise<void> started,release;
    auto signal=release.get_future().share();
    auto active=pool.submit([&] { started.set_value(); signal.wait(); return ok(); },token(),0,2,0ms);
    started.get_future().wait();
    EXPECT_FALSE(pool.submit([] { return ok(); },token(),0,1,0ms).get().success);
    EXPECT_FALSE(pool.submit([] { return ok(); },token(),0,3,0ms).get().success);
    auto producer=std::async(std::launch::async,[&] {
        return pool.submit([&] {
            const auto nested=pool.submit([] { return ok(); },token(),0,1,0ms).get();
            return algorithms::AlgoResult{!nested.success,"","{}",0};
        },token(),0,1,1s).get();
    });
    EXPECT_EQ(producer.wait_for(10ms),std::future_status::timeout);
    release.set_value(); EXPECT_TRUE(active.get().success); EXPECT_TRUE(producer.get().success);
}
TEST(JobScheduler, QueueCancellationDeadlineShutdownAndRejectedAfterStop) {
    JobScheduler pool({1,4,8}); auto cancellation=token(); std::promise<void> started;
    auto active=pool.submit([&] {
        started.set_value(); while (!cancellation->load()) std::this_thread::yield(); return ok();
    },cancellation,0,1,0ms);
    started.get_future().wait();
    auto queued=pool.submit([] { ADD_FAILURE()<<"cancelled queued task executed"; return ok(); },token(),0,1,0ms);
    auto expired=pool.submit([] { return ok(); },token(),0,1,0ms,std::chrono::steady_clock::now()-1ms);
    EXPECT_FALSE(expired.get().success);
    pool.shutdown(); EXPECT_FALSE(active.get().success); EXPECT_FALSE(queued.get().success);
    EXPECT_FALSE(pool.submit([] { return ok(); },token(),0,1,0ms).get().success);
    EXPECT_EQ(pool.stats().running,0U); EXPECT_EQ(pool.stats().reserved_bytes,0U);
}
TEST(JobScheduler, WorkerCountNeverExceedsConfigurationAndExceptionsResolve) {
    JobScheduler pool({2,32,64}); std::atomic<int> running=0,peak=0;
    std::vector<std::future<algorithms::AlgoResult>> jobs;
    for (int i=0;i<20;++i) jobs.push_back(pool.submit([&] {
        const auto n=++running; int old=peak.load(); while (n>old && !peak.compare_exchange_weak(old,n)) {}
        std::this_thread::sleep_for(1ms); --running; return ok();
    },token(),0,1,0ms));
    for (auto& job:jobs) EXPECT_TRUE(job.get().success);
    EXPECT_LE(peak,2); EXPECT_EQ(pool.stats().workers,2U);
    EXPECT_FALSE(pool.submit([]()->algorithms::AlgoResult { throw std::runtime_error("test failure"); },token(),0,1,0ms).get().success);
}
TEST(JobScheduler, ManagerOwnsAlgorithmTimeoutAndBudgets) {
    TestTempDir temp("managed_job"); nexora::core::DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,temp.path()/"graphs",{},SchedulerOptions{1,2,256*1024*1024});
    GraphDefinition def; def.name="jobs"; ASSERT_TRUE(manager.createGraph(def));
    class Job final:public algorithms::JobAlgorithm {
    public:
        std::string name() const override { return "budget_job"; }
        algorithms::AlgoResult run(const StaticGraph& snapshot,const std::vector<ExtId>& params) override {
            if (!params.empty()) for (;;) { snapshot.checkCancelled(); std::this_thread::yield(); }
            return {true,"",std::string(100,'x'),0};
        }
    };
    auto algorithm=std::make_shared<Job>(); std::weak_ptr<Job> weak=algorithm;
    auto owned=manager.submitJob("jobs",algorithm,{}); algorithm.reset(); EXPECT_TRUE(owned.result().success);
    EXPECT_TRUE(weak.expired());
    SnapshotOptions timeout; timeout.timeout_ms=10;
    const auto timed=manager.submitJob("jobs",std::make_shared<Job>(),{"wait"},timeout).result();
    EXPECT_FALSE(timed.success); EXPECT_NE(timed.error_msg.find("timeout"),std::string::npos);
    SnapshotOptions small; small.max_result_bytes=8;
    EXPECT_FALSE(manager.submitJob("jobs",std::make_shared<Job>(),{},small).result().success);
    small.max_build_bytes=1;
    EXPECT_FALSE(manager.submitJob("jobs",std::make_shared<Job>(),{},small).result().success);
    small.max_build_bytes=SIZE_MAX;
    EXPECT_FALSE(manager.submitJob("jobs",std::make_shared<Job>(),{},small).result().success);
}

TEST(JobScheduler, PriorityFloodHasBoundedBypassAndWaitTelemetry) {
    SchedulerOptions options{1,32,64}; options.max_priority_bypass=2;options.max_priority_wait_ms=60000;
    JobScheduler pool(options);std::promise<void> entered,release;
    auto signal=release.get_future().share();std::vector<int> order;
    auto active=pool.submit([&]{entered.set_value();signal.wait();return ok();},token(),0,1,0ms);
    entered.get_future().wait();
    auto low=pool.submit([&]{order.push_back(-1);return ok();},token(),-100,1,0ms);
    std::vector<std::future<algorithms::AlgoResult>> high;
    for(int i=0;i<20;++i) high.push_back(pool.submit([&,i]{order.push_back(i);return ok();},token(),100,1,0ms));
    release.set_value();EXPECT_TRUE(active.get().success);EXPECT_TRUE(low.get().success);
    for(auto& future:high) EXPECT_TRUE(future.get().success);
    ASSERT_EQ(order.size(),21U);EXPECT_EQ(order[2],-1);
    const auto stats=pool.stats();EXPECT_GE(stats.fairness_grants_total,1U);
    EXPECT_EQ(stats.admitted_total,22U);EXPECT_GT(stats.queue_ms_sum,0);EXPECT_EQ(stats.queued,0U);
}

TEST(JobScheduler, RequestControlReachesSnapshotWorkerAndReleasesCaptures) {
    TestTempDir temp("controlled_job");nexora::core::DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,temp.path()/"graphs");GraphDefinition def;def.name="g";
    ASSERT_TRUE(manager.createGraph(def));
    class Job final:public algorithms::JobAlgorithm {
    public:
        std::promise<void> entered;
        std::string name() const override {return "controlled";}
        algorithms::AlgoResult run(const StaticGraph& snapshot,const std::vector<ExtId>&) override {
            entered.set_value();for(;;) {snapshot.checkCancelled();std::this_thread::yield();}
        }
    };
    nexora::core::ExecutionScope scope(10000);scope.enter();
    auto algorithm=std::make_shared<Job>();auto entered=algorithm->entered.get_future();
    auto job=manager.submitJob("g",algorithm,{});entered.wait();scope.cancel();scope.close();
    const auto result=job.result();EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_msg.find("execution_cancelled"),std::string::npos);
    EXPECT_EQ(manager.schedulerStats().cancelled_total,1U);
    EXPECT_EQ(manager.schedulerStats().reserved_bytes,0U);
    EXPECT_TRUE(manager.prepareShutdown());
}
