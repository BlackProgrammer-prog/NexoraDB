#include "core/DocEngine.h"
#include "query/Condition.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static void require(const nexora::core::DBResult& result) {
    if (!result.success) throw std::runtime_error(result.error_msg);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--describe") {
            std::cout << Json({{"fixture_version", 2}, {"warmup_supported", true},
                               {"project_version", NEXORA_PROBE_VERSION}, {"build_type", NEXORA_PROBE_BUILD}}).dump() << '\n';
            return 0;
        }
        if (argc != 3) throw std::runtime_error("Usage: probe fixture.json FRESH_DB_PATH");
        if (std::filesystem::exists(argv[2])) throw std::runtime_error("Refusing an existing DB path");
        std::ifstream input(argv[1]);
        const Json fixture = Json::parse(input);
        const auto documents = fixture.at("documents");
        const auto trace = fixture.at("trace").get<std::vector<std::string>>();
        const auto warmup = fixture.value("warmup_operations", 10U);
        if (documents.empty() || documents.size() > 10000 || trace.size() < 10 || trace.size() > 100000)
            throw std::runtime_error("Bounded fixture/trace required");
        if (warmup > 100000) throw std::runtime_error("Warm-up exceeds bounded profile");
        nexora::core::RocksSettings settings;
        settings.durability = nexora::core::DocumentDurability::Durable;
        settings.memory_budget_bytes = 128U * 1024U * 1024U;
        nexora::core::DocEngine engine(argv[2], {}, settings);
        require(engine.CreateCollection("research_items"));
        std::unordered_map<std::string, Json> expected;
        for (const auto& document : documents) {
            const auto id = document.at("_id").get<std::string>();
            if (!expected.emplace(id, document).second) throw std::runtime_error("Duplicate fixture ID");
            require(engine.InsertOne("research_items", document.dump()));
        }
        std::vector<double> micros;
        double total_us = 0;
        std::size_t bytes = 0;
        auto lookup = [&](const std::string& id, bool measured) {
            const auto condition = nexora::query::Condition::Leaf("_id", nexora::query::Op::EQ, id);
            const auto started = Clock::now();
            const auto result = engine.FindMany("research_items", condition, 2, 0);
            const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started).count();
            require(result);
            const auto rows = Json::parse(result.data);
            if (rows != Json::array({expected.at(id)})) throw std::runtime_error("Full document oracle mismatch");
            if (measured) {
                micros.push_back(elapsed);
                total_us += elapsed;
                bytes += result.data.size();
            }
        };
        for (std::size_t i = 0; i < warmup; ++i) lookup(trace[i % trace.size()], false);
        const auto started = Clock::now();
        for (const auto& id : trace) lookup(id, true);
        const auto wall = std::chrono::duration<double>(Clock::now() - started).count();
        std::sort(micros.begin(), micros.end());
        const auto closing = Clock::now();
        require(engine.PrepareShutdown());
        const auto close_seconds = std::chrono::duration<double>(Clock::now() - closing).count();
        std::cout << Json({{"correct", true}, {"errors", 0}, {"operations", trace.size()},
            {"timed_call_ops_s", trace.size() * 1e6 / total_us}, {"ops_s", trace.size() / wall},
            {"p99_us", micros[static_cast<std::size_t>(std::ceil(.99 * micros.size())) - 1]},
            {"response_bytes", bytes}, {"close_seconds", close_seconds}, {"warmup_operations", warmup},
            {"project_version", NEXORA_PROBE_VERSION}, {"build_type", NEXORA_PROBE_BUILD},
            {"oracle_in_timed_call", false}, {"oracle_in_throughput_window", true},
            {"scope", "C++ FindMany -> JSON result; condition creation and oracle outside per-call latency"}}).dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
