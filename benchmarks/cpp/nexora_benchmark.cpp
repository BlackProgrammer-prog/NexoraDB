//
// Created by HOME on 9/1/2026.
//

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
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


namespace {
    using Clock = std::chrono::steady_clock;

    using nexora::core::DocEngine;
    using nexora::core::DBResult;
    using nexora::query::Condition;
    using nexora::query::Op;
    using nexora::query::UpdateSpec;
    using nexora::query::ValueType;

    constexpr std::uint64_t kseed = 0xC0FFEEULL;
    constexpr std::size_t kBatchSize = 4096;

}