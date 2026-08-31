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

        std::atomic<bool> finished{false};

        std::thread writer([&] {
            for (int index = 0; index < 2000; ++index) {
                graph.addNode(
                        "user_" + std::to_string(index),
                        "User"
                );
            }

            finished.store(true, std::memory_order_release);
        });

        std::vector<std::thread> readers;

        for (int reader_index = 0; reader_index < 4; ++reader_index) {
            readers.emplace_back([&] {
                while (!finished.load(std::memory_order_acquire)) {
                    static_cast<void>(graph.activeNodeCount());
                    static_cast<void>(graph.hasNode("user_0"));
                    static_cast<void>(graph.stats());
                }
            });
        }

        writer.join();

        for (auto &reader: readers) {
            reader.join();
        }

        EXPECT_EQ(graph.activeNodeCount(), 2000U);
        EXPECT_TRUE(graph.hasNode("user_0"));
        EXPECT_TRUE(graph.hasNode("user_1999"));
    }
}