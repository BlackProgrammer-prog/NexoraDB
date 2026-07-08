//
// Created by Acer on 7/9/2026.
//
#include "FriendSuggestion.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace nexora::graph::algorithms {

// ─────────────────────────────────────────────────────────────────────────────
    std::string FriendSuggestion::name() const { return "FriendSuggestion"; }

// ─────────────────────────────────────────────────────────────────────────────
    AlgoResult FriendSuggestion::run(const LiveGraph&           graph,
                                     const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();

        // ── ۱. validation ─────────────────────────────────────────────────────
        if (params.empty())
            return AlgoResult{false, "param[0] required: user_id"};

        const ExtId& user_id = params[0];

        // ── ۲. ExtId → DenseId ────────────────────────────────────────────────
        DenseId uid = graph.getDenseId(user_id);
        if (uid == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + user_id};

        // ── ۳. parse limit ────────────────────────────────────────────────────
        size_t limit = kDefaultLimit;
        if (params.size() >= 2 && !params[1].empty()) {
            try   { limit = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be a positive integer"}; }
            if (limit == 0 || limit > kMaxLimit)
                return AlgoResult{false,
                                  "limit must be between 1 and " + std::to_string(kMaxLimit)};
        }
