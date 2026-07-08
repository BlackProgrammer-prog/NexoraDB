//
// Created by Acer on 7/9/2026.
//
#include "BetweennessCentrality.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <sstream>
namespace nexora::graph::algorithms {
    
    BetweennessCentrality::BfsState::BfsState(size_t N, size_t s_idx)
    : pred(N), sigma(N, 0.0), dist(N, -1)
    {
        sigma[s_idx] = 1.0;
        dist[s_idx]  = 0;
        queue.push_back(s_idx);
    }