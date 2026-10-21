// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// Cumulative counters only: no sample values, recordings or instance addresses.
struct TetrisBessoundProbeStats {
    bool compatible;
    uint64_t scopedCalls;
    uint64_t monoCalls;
    uint64_t stereoCalls;
    uint64_t unknownCalls;
    uint64_t inputFrames;
    uint64_t inputDifferent;
    uint64_t outputFrames;
    uint64_t outputDifferent;
    uint64_t totalCalls;
    uint64_t unscopedCalls;
    bool originalResolved;
    bool wrapperMatched;
    bool engineMatched;
};
extern "C" TetrisBessoundProbeStats tetris_bessound_probe_stats();
