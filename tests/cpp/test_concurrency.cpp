#include "graph/Livegraph.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace nexora::graph;

    TEST(LiveGraphConcurrency, ConcurrentReadersAndWriter) {
        LiveGraph graph(nullptr, nullptr, "concurrent");

        std::atomic<bool> start{false};

        std::thread writer([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int index = 0; index < 2000; ++index) {
                graph.addNode(
                        "user_" + std::to_string(index),
                        "User"
                );
            }
        });

        std::vector<std::thread> readers;

        for (int reader_index = 0; reader_index < 4; ++reader_index) {
            readers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                // Keep the workload bounded. A loop tied to writer completion can
                // starve unique_lock indefinitely on reader-preferring shared_mutex
                // implementations.
                for (int iteration = 0; iteration < 250; ++iteration) {
                    static_cast<void>(graph.activeNodeCount());
                    static_cast<void>(graph.hasNode("user_0"));
                    static_cast<void>(graph.stats());
                    std::this_thread::yield();
                }
            });
        }

        start.store(true, std::memory_order_release);

        writer.join();

        for (auto &reader: readers) {
            reader.join();
        }

        EXPECT_EQ(graph.activeNodeCount(), 2000U);
        EXPECT_TRUE(graph.hasNode("user_0"));
        EXPECT_TRUE(graph.hasNode("user_1999"));
    }
}
