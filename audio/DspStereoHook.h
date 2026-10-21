// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// Synchronous, caller-owned scope around a prospective eligible media write.
struct TetrisDspStereoScope {
    TetrisDspStereoScope* previous = nullptr;
    void* owner = nullptr;
    uint64_t generation = 0;
    bool eligible = false;
};
struct TetrisDspStereoState {
    bool compatible;
    bool dirty;
    // Accepted INIT transport, returned objects and successful task open;
    // this is not a firmware-mode or acoustic readback.
    bool ready;
    bool fault;
    void* owner;
    void* handler;
    uint64_t initAttempts;
    uint64_t initAccepted;
    uint64_t teardowns;
    // Bounded diagnostics only: no PCM, addresses or additional vendor calls.
    uint64_t eligibleWrites;
    uint64_t observedOpens;
    uint64_t eligibleOpens;
    uint64_t freshCreates;
    uint64_t initPackets;
    const char* lastGate;
    uint32_t packetPlatform;
    uint32_t packetScenario;
    uint32_t packetDirection;
    uint64_t handlerCount;
    // Last completed eligible open, retained across unrelated opens/teardown.
    // INT32_MIN means no eligible open has returned yet.
    int32_t lastEligibleOpenResult;
};
extern "C" bool tetris_dsp_stereo_initialize(void* vendorHandle);
extern "C" bool tetris_dsp_stereo_compatible();
extern "C" void tetris_dsp_stereo_begin(TetrisDspStereoScope*, void* owner, bool eligible);
extern "C" void tetris_dsp_stereo_end(TetrisDspStereoScope*);
// Revoke admission/readiness, retaining the dirty owner until verified teardown.
extern "C" void tetris_dsp_stereo_revoke();
// Latch an unresolved lifecycle failure; there is deliberately no reset API.
extern "C" void tetris_dsp_stereo_fault();
extern "C" TetrisDspStereoState tetris_dsp_stereo_state();
