//
// Created by Acer on 7/9/2026.
//
#include "InfluenceMaximization.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
namespace nexora::graph::algorithms {
    
    std::string InfluenceMaximization::name() const {
        return "InfluenceMaximization";
    }

    AlgoResult InfluenceMaximization::run(const StaticGraph&         snapshot,
                                          const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();

        size_t K = kDefaultK;
        size_t R = kDefaultR;
        double p = kDefaultP;

        if (!params.empty() && !params[0].empty()) {
            try   { K = static_cast<size_t>(std::stoul(params[0])); }
            catch (...) { return AlgoResult{false, "param[0] must be positive integer (K)"}; }
        }
        if (params.size() >= 2 && !params[1].empty()) {
            try   { R = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be positive integer (R)"}; }
        }
        if (K == 0) return AlgoResult{false, "K must be >= 1"};
        if (R == 0) return AlgoResult{false, "R must be >= 1"};
