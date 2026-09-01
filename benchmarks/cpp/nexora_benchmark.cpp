#include "core/DocEngine.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

#ifdef NEXORA_BUILD_GRAPH
#include "graph/Livegraph.h"
#include "graph/StaticGraph.h"
#include "graph/algorithms/ShortestPath.h"
#endif

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// CMake supplies these values for the nexora_benchmark target.  The fallbacks
// keep standalone IDE/indexer parsing valid without overriding configured
// build metadata.
#ifndef NEXORA_BENCH_PROJECT_VERSION
#define NEXORA_BENCH_PROJECT_VERSION "unknown"
#endif

#ifndef NEXORA_BENCH_BUILD_TYPE
#define NEXORA_BENCH_BUILD_TYPE "unknown"
#endif

#ifndef NEXORA_BENCH_COMPILER_ID
#define NEXORA_BENCH_COMPILER_ID "unknown"
#endif

#ifndef NEXORA_BENCH_COMPILER_VERSION
#define NEXORA_BENCH_COMPILER_VERSION "unknown"
#endif

#ifndef NEXORA_BENCH_ROCKSDB_VERSION
#define NEXORA_BENCH_ROCKSDB_VERSION "unknown"
#endif

#ifndef NEXORA_BENCH_SYSTEM
#define NEXORA_BENCH_SYSTEM "unknown"
#endif

namespace {

    using Clock = std::chrono::steady_clock;

    using nexora::core::DBResult;
    using nexora::core::DocEngine;
    using nexora::query::Condition;
    using nexora::query::Op;
    using nexora::query::UpdateSpec;
    using nexora::query::ValueType;

    constexpr std::uint64_t kSeed = 0xC0FFEEULL;
    constexpr std::size_t kBatchSize = 4096;

    std::atomic<std::uint64_t> g_path_sequence{0};

    double secondsBetween(
            const Clock::time_point begin,
            const Clock::time_point end
    ) {
        return std::chrono::duration<double>(end - begin).count();
    }

    std::uint64_t splitMix64(std::uint64_t value) {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::string documentId(const std::uint64_t index) {
        return "doc_" + std::to_string(index);
    }

    std::string makeDocument(
            const std::uint64_t index,
            const std::uint64_t seed = kSeed
    ) {
        const auto random = splitMix64(seed ^ index);
        const auto age = 18ULL + random % 73ULL;
        const auto score = random % 1'000'000ULL;
        const auto group = index % 1000ULL;

        return "{\"_id\":\"" + documentId(index) +
               "\",\"age\":" + std::to_string(age) +
               ",\"score\":" + std::to_string(score) +
               ",\"group\":\"group_" + std::to_string(group) +
               "\",\"active\":true}";
    }

    std::filesystem::path makeTemporaryPath(
            const std::string& workload,
            const std::size_t size
    ) {
        const auto sequence =
                g_path_sequence.fetch_add(1, std::memory_order_relaxed);

        return std::filesystem::temp_directory_path() /
               ("nexora_benchmark_" + workload + "_" +
                std::to_string(size) + "_" +
                std::to_string(sequence));
    }

    void reportFailure(
            benchmark::State& state,
            const DBResult& result
    ) {
        const std::string message = result.error_msg.empty()
                                    ? "NexoraDB operation failed"
                                    : result.error_msg;

        state.SkipWithError(message.c_str());
    }

    class DocumentDataset {
    public:
        DocumentDataset(
                const std::string& workload,
                const std::size_t size,
                const bool populate
        )
                : path_(makeTemporaryPath(workload, size)),
                  size_(size) {
            std::filesystem::remove_all(path_);

            engine_ = std::make_unique<DocEngine>(path_.string());

            const auto created = engine_->CreateCollection("documents");
            if (!created.success) {
                throw std::runtime_error(created.error_msg);
            }

            if (populate) {
                load();
            }
        }

        ~DocumentDataset() {
            engine_.reset();

            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        DocEngine& engine() {
            return *engine_;
        }

    private:
        void load() {
            std::vector<std::string> batch;
            batch.reserve(kBatchSize);

            for (std::size_t index = 0; index < size_; ++index) {
                batch.push_back(makeDocument(index));

                if (batch.size() == kBatchSize || index + 1 == size_) {
                    const auto result =
                            engine_->InsertMany("documents", batch);

                    if (!result.success) {
                        throw std::runtime_error(result.error_msg);
                    }

                    batch.clear();
                }
            }
        }

        std::filesystem::path path_;
        std::size_t size_;
        std::unique_ptr<DocEngine> engine_;
    };

    void standardDatasetSizes(benchmark::internal::Benchmark* benchmark) {
        benchmark
                ->ArgName("documents")
                ->Arg(100'000)
                ->Arg(1'000'000);

        // دیتاست 10M فقط به‌صورت opt-in ثبت می‌شود.
        const char* enable10m = std::getenv("NEXORA_BENCH_10M");
        if (enable10m != nullptr && std::string(enable10m) == "1") {
            benchmark->Arg(10'000'000);
        }
    }

// ---------------------------------------------------------
// Document: point read
// ---------------------------------------------------------

    void BM_DocumentPointRead(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("point_read", size, true);

        std::uint64_t iteration = 0;

        for (auto _ : state) {
            const auto random = splitMix64(kSeed ^ iteration++);
            const auto id = documentId(random % size);

            const auto begin = Clock::now();
            const auto result = dataset.engine().FindById("documents", id);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(state.iterations());
    }

// ---------------------------------------------------------
// Document: FindMany equality
// ---------------------------------------------------------

    void BM_DocumentFindManyEquality(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("find_equality", size, true);

        const auto condition = Condition::Leaf(
                "group",
                Op::EQ,
                "group_42",
                ValueType::String
        );

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().FindMany("documents", condition);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }
    }

// ---------------------------------------------------------
// Document: FindMany numeric range
// ---------------------------------------------------------

    void BM_DocumentFindManyRange(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("find_range", size, true);

        const auto condition = Condition::And({
                                                      Condition::Leaf(
                                                              "score",
                                                              Op::GTE,
                                                              "250000",
                                                              ValueType::Int64
                                                      ),
                                                      Condition::Leaf(
                                                              "score",
                                                              Op::LT,
                                                              "750000",
                                                              ValueType::Int64
                                                      )
                                              });

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().FindMany("documents", condition);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }
    }

// ---------------------------------------------------------
// Document: full scan
// ---------------------------------------------------------

    void BM_DocumentFullScan(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("full_scan", size, true);

        const Condition matchAll;

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().FindMany("documents", matchAll);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(
                state.iterations() * static_cast<std::int64_t>(size)
        );
    }

// ---------------------------------------------------------
// Document: InsertOne
// ---------------------------------------------------------

    void BM_DocumentInsertOne(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("insert_one", size, false);

        for (auto _ : state) {
            double elapsed = 0.0;

            for (std::size_t index = 0; index < size; ++index) {
                const auto document = makeDocument(index);

                const auto begin = Clock::now();
                const auto result =
                        dataset.engine().InsertOne("documents", document);
                const auto end = Clock::now();

                if (!result.success) {
                    reportFailure(state, result);
                    return;
                }

                elapsed += secondsBetween(begin, end);
            }

            state.SetIterationTime(elapsed);
        }

        state.SetItemsProcessed(static_cast<std::int64_t>(size));
    }

// ---------------------------------------------------------
// Document: InsertMany
// ---------------------------------------------------------

    void BM_DocumentInsertBatch(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("insert_batch", size, false);

        for (auto _ : state) {
            double elapsed = 0.0;
            std::vector<std::string> batch;
            batch.reserve(kBatchSize);

            for (std::size_t index = 0; index < size; ++index) {
                batch.push_back(makeDocument(index));

                if (batch.size() == kBatchSize || index + 1 == size) {
                    const auto begin = Clock::now();
                    const auto result =
                            dataset.engine().InsertMany("documents", batch);
                    const auto end = Clock::now();

                    if (!result.success) {
                        reportFailure(state, result);
                        return;
                    }

                    elapsed += secondsBetween(begin, end);
                    batch.clear();
                }
            }

            state.SetIterationTime(elapsed);
        }

        state.SetItemsProcessed(static_cast<std::int64_t>(size));
    }

// ---------------------------------------------------------
// Document: single update
// ---------------------------------------------------------

    void BM_DocumentUpdateById(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("update_by_id", size, true);

        UpdateSpec update;
        update.Inc("score", "1");

        std::uint64_t index = 0;

        for (auto _ : state) {
            const auto id = documentId(index++ % size);

            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().UpdateById("documents", id, update);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(state.iterations());
    }

// ---------------------------------------------------------
// Document: group update
// ---------------------------------------------------------

    void BM_DocumentUpdateMany(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("update_many", size, true);

        const auto condition = Condition::Leaf(
                "group",
                Op::EQ,
                "group_42"
        );

        UpdateSpec update;
        update.Set("active", "false");

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().UpdateMany(
                            "documents",
                            condition,
                            update
                    );
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }
    }

// ---------------------------------------------------------
// Document: single delete
// ---------------------------------------------------------

    void BM_DocumentDeleteById(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("delete_by_id", size, true);

        std::uint64_t index = 0;

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result = dataset.engine().DeleteById(
                    "documents",
                    documentId(index++ % size)
            );
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(state.iterations());
    }

// ---------------------------------------------------------
// Document: group delete
// ---------------------------------------------------------

    void BM_DocumentDeleteMany(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        DocumentDataset dataset("delete_many", size, true);

        const auto condition = Condition::Leaf(
                "group",
                Op::EQ,
                "group_42"
        );

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    dataset.engine().DeleteMany("documents", condition);
            const auto end = Clock::now();

            if (!result.success) {
                reportFailure(state, result);
                break;
            }

            benchmark::DoNotOptimize(result.data.data());
            benchmark::DoNotOptimize(result.data.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }
    }

#ifdef NEXORA_BUILD_GRAPH

    using nexora::graph::DenseId;
    using nexora::graph::Direction;
    using nexora::graph::LiveGraph;
    using nexora::graph::StaticGraph;
    using nexora::graph::algorithms::ShortestPath;

    std::string userId(const std::size_t index) {
        return "user_" + std::to_string(index);
    }

    class GraphDataset {
    public:
        explicit GraphDataset(const std::size_t size)
                : graph_(nullptr, nullptr, "benchmark"),
                  size_(size) {
            load();
        }

        LiveGraph& graph() {
            return graph_;
        }

    private:
        void load() {
            for (std::size_t index = 0; index < size_; ++index) {
                graph_.addNode(userId(index), "User");
            }

            for (std::size_t index = 0; index < size_; ++index) {
                const auto next = (index + 1) % size_;
                const auto random =
                        splitMix64(kSeed ^ index) % size_;

                graph_.addEdge(
                        userId(index),
                        userId(next),
                        "FOLLOWS",
                        true
                );

                if (random != index && random != next) {
                    graph_.addEdge(
                            userId(index),
                            userId(random),
                            "FOLLOWS",
                            true
                    );
                }
            }
        }

        LiveGraph graph_;
        std::size_t size_;
    };

// ---------------------------------------------------------
// Graph: bulk node and edge ingestion
// ---------------------------------------------------------

    void BM_GraphBulkIngestion(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));

        for (auto _ : state) {
            LiveGraph graph(nullptr, nullptr, "bulk_ingestion");

            const auto begin = Clock::now();

            for (std::size_t index = 0; index < size; ++index) {
                graph.addNode(userId(index), "User");
            }

            for (std::size_t index = 0; index < size; ++index) {
                graph.addEdge(
                        userId(index),
                        userId((index + 1) % size),
                        "FOLLOWS",
                        true
                );
            }

            const auto end = Clock::now();

            benchmark::DoNotOptimize(graph.activeNodeCount());
            benchmark::DoNotOptimize(graph.activeEdgeCount());
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(
                static_cast<std::int64_t>(size * 2)
        );
    }

// ---------------------------------------------------------
// Graph: neighbor lookup
// ---------------------------------------------------------

    void BM_GraphNeighborLookup(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        GraphDataset dataset(size);

        const DenseId node =
                dataset.graph().getDenseId(userId(size / 2));

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto neighbors = dataset.graph().neighbors(
                    node,
                    Direction::Out
            );
            const auto end = Clock::now();

            benchmark::DoNotOptimize(neighbors.data());
            benchmark::DoNotOptimize(neighbors.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(state.iterations());
    }

// ---------------------------------------------------------
// Graph: shortest path
// ---------------------------------------------------------

    void BM_GraphShortestPath(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        GraphDataset dataset(size);

        ShortestPath algorithm;

        const std::vector<std::string> parameters{
                userId(0),
                userId(std::min<std::size_t>(10, size - 1)),
                "FOLLOWS"
        };

        for (auto _ : state) {
            const auto begin = Clock::now();
            const auto result =
                    algorithm.run(dataset.graph(), parameters);
            const auto end = Clock::now();

            if (!result.success) {
                state.SkipWithError(result.error_msg.c_str());
                break;
            }

            benchmark::DoNotOptimize(result.result_json.data());
            benchmark::DoNotOptimize(result.result_json.size());
            state.SetIterationTime(secondsBetween(begin, end));
        }
    }

// ---------------------------------------------------------
// Graph: snapshot build
// ---------------------------------------------------------

    void BM_GraphSnapshotBuild(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        GraphDataset dataset(size);

        for (auto _ : state) {
            const auto begin = Clock::now();
            StaticGraph snapshot(dataset.graph(), "benchmark_snapshot");
            const auto end = Clock::now();

            benchmark::DoNotOptimize(snapshot.nodeCount());
            benchmark::DoNotOptimize(snapshot.edgeCount());
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(
                static_cast<std::int64_t>(
                        dataset.graph().activeNodeCount() +
                        dataset.graph().activeEdgeCount()
                )
        );
    }

// ---------------------------------------------------------
// Graph: concurrent reads and writes
// ---------------------------------------------------------

    void BM_GraphConcurrentReadWrite(benchmark::State& state) {
        const auto size = static_cast<std::size_t>(state.range(0));
        GraphDataset dataset(size);

        std::uint64_t round = 0;

        for (auto _ : state) {
            std::atomic<bool> start{false};
            std::vector<std::thread> readers;

            const auto begin = Clock::now();

            for (int readerIndex = 0; readerIndex < 4; ++readerIndex) {
                readers.emplace_back([&] {
                    while (!start.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }

                    for (std::size_t operation = 0;
                         operation < 250;
                         ++operation) {
                        const auto node =
                                static_cast<DenseId>(operation % size);

                        const auto neighbors = dataset.graph().neighbors(
                                node,
                                Direction::Out
                        );

                        benchmark::DoNotOptimize(neighbors.data());
                        benchmark::DoNotOptimize(neighbors.size());
                    }
                });
            }

            std::thread writer([&] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                const auto base = size + round * 100;

                for (std::size_t operation = 0;
                     operation < 100;
                     ++operation) {
                    const auto id = userId(base + operation);

                    dataset.graph().addNode(id, "User");
                    dataset.graph().addEdge(
                            id,
                            userId(operation % size),
                            "FOLLOWS",
                            true
                    );
                }
            });

            start.store(true, std::memory_order_release);

            writer.join();
            for (auto& reader : readers) {
                reader.join();
            }

            const auto end = Clock::now();

            ++round;
            state.SetIterationTime(secondsBetween(begin, end));
        }

        state.SetItemsProcessed(
                state.iterations() * 1100
        );
    }

#endif

    BENCHMARK(BM_DocumentPointRead)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

    BENCHMARK(BM_DocumentFindManyEquality)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

    BENCHMARK(BM_DocumentFindManyRange)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

    BENCHMARK(BM_DocumentFullScan)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_DocumentInsertOne)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_DocumentInsertBatch)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_DocumentUpdateById)
            ->Apply(standardDatasetSizes)
            ->Iterations(1000)
            ->UseManualTime();

    BENCHMARK(BM_DocumentUpdateMany)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_DocumentDeleteById)
            ->Apply(standardDatasetSizes)
            ->Iterations(1000)
            ->UseManualTime();

    BENCHMARK(BM_DocumentDeleteMany)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

#ifdef NEXORA_BUILD_GRAPH

    BENCHMARK(BM_GraphBulkIngestion)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_GraphNeighborLookup)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

    BENCHMARK(BM_GraphShortestPath)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

    BENCHMARK(BM_GraphSnapshotBuild)
            ->Apply(standardDatasetSizes)
            ->Iterations(1)
            ->UseManualTime();

    BENCHMARK(BM_GraphConcurrentReadWrite)
            ->Apply(standardDatasetSizes)
            ->UseManualTime();

#endif

} // namespace

int main(int argc, char** argv) {
    benchmark::AddCustomContext("nexora_version",
                                NEXORA_BENCH_PROJECT_VERSION);
    benchmark::AddCustomContext("build_type",
                                NEXORA_BENCH_BUILD_TYPE);
    benchmark::AddCustomContext("compiler",
                                NEXORA_BENCH_COMPILER_ID);
    benchmark::AddCustomContext("compiler_version",
                                NEXORA_BENCH_COMPILER_VERSION);
    benchmark::AddCustomContext("rocksdb_version",
                                NEXORA_BENCH_ROCKSDB_VERSION);
    benchmark::AddCustomContext("system",
                                NEXORA_BENCH_SYSTEM);
    benchmark::AddCustomContext("dataset_seed",
                                std::to_string(kSeed));

    benchmark::Initialize(&argc, argv);

    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
