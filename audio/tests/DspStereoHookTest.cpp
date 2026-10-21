// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
static void* resolveTestSymbol(void*, const char*);
#define dlsym resolveTestSymbol
#include "../DspStereoHook.cpp"
#undef dlsym

namespace {
using dsp_stereo::read;
using dsp_stereo::write;
struct Manager {
    alignas(void*) std::array<unsigned char, 0xc00> bytes{};
    std::array<void*, 2> entry{};
    void* self() { return bytes.data(); }
    void** managerSlot() { return reinterpret_cast<void**>(bytes.data() + 0xbd8); }
    void** configSlot() { return reinterpret_cast<void**>(bytes.data() + 0xbe0); }
    void prepare(void* handler, size_t count = 1) {
        bytes.fill(0);
        entry = {handler, handler};
        write<void*>(self(), 0x268, entry.data());
        write<size_t>(self(), 0x270, count);
    }
};
Manager managerA, managerB;
int ownerA, ownerB, handlerA, handlerB, objectA, configA, inputAttr, outputAttr;
uint32_t scenario = 4;
uint32_t packetScenarioOverride = UINT32_MAX;
uint8_t task = 32, direction = 1;
uint32_t outputDevice = 2, secondOutput = 0;
uint8_t platform = 2;
size_t packetMutationOffset = SIZE_MAX;
uint32_t packetBytes = 0x340, transportParam2 = 0;
uint8_t transportLayer = 2, transportDataType = 2, transportAck = 1;
bool nullPayload = false;
int sendResult = 0, openResult = 0;
bool returnObjects = true, clearObjects = true, sendDestroy = true;
bool skipCreate = false;
bool initSawStereo = false, restoredPacket = false, nestedSend = false;
bool omitCreateSymbol = false, selfSendSymbol = false;
unsigned rawCalls = 0;
std::mutex pauseMutex;
std::condition_variable pauseCv;
bool pauseBeforeSend = false, pauseInsideSend = false, entered = false, resume = false;

void waitForResume() {
    std::unique_lock lock(pauseMutex);
    entered = true;
    pauseCv.notify_all();
    pauseCv.wait(lock, [] { return resume; });
}
std::array<unsigned char, 0x340> packet() {
    std::array<unsigned char, 0x340> data{};
    write<uint32_t>(data.data(), 0, 0x55667788);
    write<uint32_t>(data.data(), 0x31c, 0xaabbccdd);
    write<uint32_t>(data.data(), 0x6c,
            packetScenarioOverride == UINT32_MAX ? scenario : packetScenarioOverride);
    write<uint8_t>(data.data(), 0x70, platform);
    write<uint8_t>(data.data(), 0x71, direction);
    write<uint8_t>(data.data(), 0x44, task);
    write<uint8_t>(data.data(), 0x17c, task);
    write<uint32_t>(data.data(), 0x24, outputDevice);
    write<uint32_t>(data.data(), 0x28, secondOutput);
    write<uint16_t>(data.data(), 0x42, 0x108); // Preserve smart-PA and unrelated flags.
    if (packetMutationOffset < data.size()) data[packetMutationOffset] ^= 1;
    return data;
}
int mockSend(void*, uint8_t sentTask, uint8_t, uint8_t, uint8_t,
             uint16_t id, uint32_t, uint32_t, void* data) {
    ++rawCalls;
    if (id == 0x15 && data) {
        initSawStereo = (read<uint16_t>(data, 0x42) & 0x4) != 0;
        if (initSawStereo) {
            assert(read<uint16_t>(data, 0x42) == 0x10c);
            assert(read<uint32_t>(data, 0x6c) == scenario);
        }
        if (nestedSend) {
            nestedSend = false;
            auto foreign = packet();
            const auto saved = foreign;
            assert(tetris_dsp_vendor_send(nullptr, sentTask, 2, 2, 1, 0x18,
                    foreign.size(), 0, foreign.data()) == sendResult);
            assert(foreign == saved);
        }
        if (pauseInsideSend) waitForResume();
    }
    return sendResult;
}
void mockCreate(void*, void** managerSlot, void** configSlot, uint8_t sentTask,
                uint32_t sentScenario, uint8_t, uint32_t, const void*, const void*, const void*, const void*) {
    assert(sentScenario == scenario);
    if (*managerSlot || *configSlot) return;
    auto data = packet();
    const auto saved = data;
    if (pauseBeforeSend) waitForResume();
    assert(tetris_dsp_vendor_send(nullptr, sentTask, transportLayer, transportDataType, transportAck,
            0x15, packetBytes, transportParam2, nullPayload ? nullptr : data.data()) == sendResult);
    restoredPacket = data == saved;
    assert(restoredPacket);
    if (returnObjects) { *managerSlot = &objectA; *configSlot = &configA; }
}
int mockOpen(void* self, void*) {
    if (skipCreate) return openResult;
    auto* base = static_cast<unsigned char*>(self);
    tetris_dsp_vendor_create(self, reinterpret_cast<void**>(base + 0xbd8),
            reinterpret_cast<void**>(base + 0xbe0), task, scenario, direction, 0,
            &inputAttr, &outputAttr, nullptr, nullptr);
    return openResult;
}
void mockDestroy(void*, void** managerSlot, void** configSlot, uint8_t sentTask) {
    if (sendDestroy) {
        assert(tetris_dsp_vendor_send(nullptr, sentTask, 2, 0, 1, 0x16, 0, 0, nullptr) == sendResult);
    }
    if (clearObjects) *managerSlot = *configSlot = nullptr;
}
void reset() {
    using namespace dsp_stereo;
    assert(!writeScope && !openScope && !createScope && !destroyScope);
    state = {};
    originalOpen.store(mockOpen);
    originalCreate.store(mockCreate);
    originalSend.store(mockSend);
    originalDestroy.store(mockDestroy);
    compatible.store(true);
    managerA.prepare(&handlerA);
    managerB.prepare(&handlerB);
    scenario = 4; task = 32; direction = 1; outputDevice = 2; secondOutput = 0; platform = 2;
    packetScenarioOverride = UINT32_MAX;
    packetMutationOffset = SIZE_MAX;
    packetBytes = 0x340; transportParam2 = 0;
    transportLayer = transportDataType = 2; transportAck = 1;
    nullPayload = false;
    sendResult = openResult = 0;
    returnObjects = clearObjects = sendDestroy = true;
    skipCreate = false;
    initSawStereo = restoredPacket = nestedSend = false;
    omitCreateSymbol = selfSendSymbol = false;
    rawCalls = 0;
    pauseBeforeSend = pauseInsideSend = entered = resume = false;
}
int openOwned(Manager& manager = managerA, void* owner = &ownerA, void* handler = &handlerA,
              bool eligible = true) {
    TetrisDspStereoScope scope;
    tetris_dsp_stereo_begin(&scope, owner, eligible);
    int result = tetris_dsp_vendor_open(manager.self(), handler);
    tetris_dsp_stereo_end(&scope);
    return result;
}
void destroyOwned() {
    tetris_dsp_vendor_destroy(managerA.self(), managerA.managerSlot(), managerA.configSlot(), 32);
}
void assertStock() {
    openOwned();
    assert(!initSawStereo && !tetris_dsp_stereo_state().dirty && !tetris_dsp_stereo_state().ready);
}
}

static void* resolveTestSymbol(void*, const char* name) {
    using namespace dsp_stereo;
    if (std::strcmp(name, kOpen) == 0) return reinterpret_cast<void*>(mockOpen);
    if (std::strcmp(name, kCreate) == 0) return omitCreateSymbol ? nullptr : reinterpret_cast<void*>(mockCreate);
    if (std::strcmp(name, kDestroy) == 0) return reinterpret_cast<void*>(mockDestroy);
    if (std::strcmp(name, kSend) == 0) return selfSendSymbol ? reinterpret_cast<void*>(tetris_dsp_vendor_send)
            : reinterpret_cast<void*>(mockSend);
    return nullptr;
}

int main() {
    // Preserve an eligible vendor failure before INIT even if a later stock
    // open overwrites lastGate. Diagnostics never authorize an INIT or retry.
    reset();
    assert(tetris_dsp_stereo_state().lastEligibleOpenResult == INT32_MIN);
    skipCreate = true;
    openResult = -3;
    assert(openOwned() == -3);
    auto failedOpen = tetris_dsp_stereo_state();
    assert(failedOpen.lastEligibleOpenResult == -3 && failedOpen.eligibleOpens == 1);
    assert(failedOpen.freshCreates == 0 && failedOpen.initAttempts == 0 && rawCalls == 0);
    assert(!failedOpen.dirty && !failedOpen.ready && !failedOpen.fault);
    openResult = 0;
    assert(openOwned(managerB, &ownerB, &handlerB, false) == 0);
    failedOpen = tetris_dsp_stereo_state();
    assert(std::strcmp(failedOpen.lastGate, "open_scope") == 0);
    assert(failedOpen.lastEligibleOpenResult == -3 && failedOpen.eligibleOpens == 1);
    skipCreate = false;
    assert(openOwned() == 0);
    assert(tetris_dsp_stereo_state().lastEligibleOpenResult == 0);
    destroyOwned();
    assert(tetris_dsp_stereo_state().lastEligibleOpenResult == 0);

    // Diagnostics distinguish proxy admission, manager exclusivity and packet
    // layout without admitting a write or changing rejected stock packets.
    reset();
    openOwned(managerA, &ownerA, &handlerA, false);
    auto diagnostics = tetris_dsp_stereo_state();
    assert(diagnostics.eligibleWrites == 0 && diagnostics.observedOpens == 1);
    assert(diagnostics.eligibleOpens == 0 && diagnostics.freshCreates == 0 && diagnostics.initPackets == 0);
    assert(diagnostics.lastEligibleOpenResult == INT32_MIN);
    assert(std::strcmp(diagnostics.lastGate, "open_scope") == 0 && !diagnostics.dirty);
    reset();
    managerA.prepare(&handlerA, 2);
    openOwned();
    diagnostics = tetris_dsp_stereo_state();
    assert(diagnostics.eligibleWrites == 1 && diagnostics.observedOpens == 1 && diagnostics.handlerCount == 2);
    assert(diagnostics.eligibleOpens == 0 && diagnostics.freshCreates == 0 && diagnostics.initPackets == 0);
    assert(std::strcmp(diagnostics.lastGate, "open_handler") == 0 && !diagnostics.dirty);
    reset();
    platform = 1;
    openOwned();
    diagnostics = tetris_dsp_stereo_state();
    assert(diagnostics.eligibleWrites == 1 && diagnostics.eligibleOpens == 1 && diagnostics.handlerCount == 1);
    assert(diagnostics.freshCreates == 1 && diagnostics.initPackets == 1);
    assert(diagnostics.packetPlatform == 1 && diagnostics.packetScenario == 4 && diagnostics.packetDirection == 1);
    assert(std::strcmp(diagnostics.lastGate, "packet_layout") == 0 && !diagnostics.dirty && restoredPacket);
    reset();
    scenario = 11;
    openOwned();
    diagnostics = tetris_dsp_stereo_state();
    assert(diagnostics.eligibleOpens == 1 && diagnostics.freshCreates == 0 && diagnostics.initPackets == 0);
    assert(std::strcmp(diagnostics.lastGate, "create_scenario") == 0);
    reset();
    openOwned();
    diagnostics = tetris_dsp_stereo_state();
    assert(diagnostics.eligibleWrites == 1 && diagnostics.observedOpens == 1 && diagnostics.eligibleOpens == 1);
    assert(diagnostics.freshCreates == 1 && diagnostics.initPackets == 1 && diagnostics.initAttempts == 1);
    assert(diagnostics.packetPlatform == 2 && diagnostics.packetScenario == 4 && diagnostics.packetDirection == 1);
    assert(std::strcmp(diagnostics.lastGate, "ready") == 0 && diagnostics.ready && restoredPacket);

    // Both vendor-selected playback scenarios use the same owned task and
    // teardown. The SmartPA flag and the original scenario must survive INIT.
    for (uint32_t playback : {4u, 10u}) {
        reset();
        scenario = playback;
        assert(openOwned() == 0);
        diagnostics = tetris_dsp_stereo_state();
        assert(diagnostics.ready && diagnostics.dirty && !diagnostics.fault);
        assert(diagnostics.freshCreates == 1 && diagnostics.initAttempts == 1 && diagnostics.initAccepted == 1);
        assert(diagnostics.packetScenario == playback && initSawStereo && restoredPacket);
        destroyOwned();
        diagnostics = tetris_dsp_stereo_state();
        assert(!diagnostics.dirty && !diagnostics.ready && !diagnostics.fault && diagnostics.teardowns == 1);

        // Accepting either scenario is insufficient: INIT must match this
        // particular Create invocation, even when the other is supported.
        reset();
        scenario = playback;
        packetScenarioOverride = playback == 4 ? 10 : 4;
        assertStock();
        diagnostics = tetris_dsp_stereo_state();
        assert(diagnostics.freshCreates == 1 && diagnostics.initPackets == 1 && diagnostics.initAttempts == 0);
        assert(diagnostics.packetScenario == packetScenarioOverride && restoredPacket && rawCalls == 1);
        assert(std::strcmp(diagnostics.lastGate, "packet_layout") == 0);
    }
    for (uint32_t unsupported : {0u, 3u, 5u, 9u, 11u, UINT32_MAX}) {
        reset();
        scenario = unsupported;
        assertStock();
        diagnostics = tetris_dsp_stereo_state();
        assert(diagnostics.freshCreates == 0 && diagnostics.initAttempts == 0 && restoredPacket);
        assert(std::strcmp(diagnostics.lastGate, "create_scenario") == 0);
    }

    // Partial symbol resolution must retain all independently available stock
    // callbacks, and any failed reinitialization must clear compatibility.
    reset();
    omitCreateSymbol = true;
    dsp_stereo::originalSend.store(nullptr);
    assert(!tetris_dsp_stereo_initialize(reinterpret_cast<void*>(1)));
    assert(!tetris_dsp_stereo_compatible());
    assert(dsp_stereo::originalOpen.load() == mockOpen);
    assert(dsp_stereo::originalCreate.load() == nullptr);
    assert(dsp_stereo::originalDestroy.load() == mockDestroy);
    assert(dsp_stereo::originalSend.load() == mockSend);
    assert(tetris_dsp_vendor_send(nullptr, 31, 2, 0, 1, 0x16, 0, 0, nullptr) == 0 && rawCalls == 1);
    reset();
    selfSendSymbol = true;
    assert(!tetris_dsp_stereo_initialize(reinterpret_cast<void*>(1)));
    assert(!tetris_dsp_stereo_compatible() && dsp_stereo::originalSend.load() == nullptr);
    assert(dsp_stereo::originalOpen.load() == mockOpen);
    reset();
    nestedSend = true;
    assert(openOwned() == 0);
    auto status = tetris_dsp_stereo_state();
    assert(status.dirty && status.ready && !status.fault && status.owner == &ownerA);
    assert(status.handler == &handlerA && status.initAttempts == 1 && status.initAccepted == 1);
    assert(initSawStereo && restoredPacket && rawCalls == 2);
    tetris_dsp_stereo_revoke();
    status = tetris_dsp_stereo_state();
    assert(status.dirty && !status.ready && status.owner == &ownerA);
    destroyOwned();
    status = tetris_dsp_stereo_state();
    assert(!status.dirty && !status.owner && !status.handler && status.teardowns == 1 && !status.fault);

    // Rejected admission must not inspect arbitrary vendor pointers.
    reset();
    dsp_stereo::compatible.store(false);
    assertStock();
    assert(!dsp_stereo::knownFunction(nullptr, 1, 1));
    assert(!dsp_stereo::knownFunction(reinterpret_cast<void*>(1), 1, 1));
    assert(!dsp_stereo::knownFunction(reinterpret_cast<void*>(mockOpen), 16, 0));
#if defined(__aarch64__)
    uint64_t fingerprint = 14695981039346656037ULL;
    const auto* code = reinterpret_cast<const unsigned char*>(mockOpen);
    for (size_t i = 0; i < 16; ++i) fingerprint = (fingerprint ^ code[i]) * 1099511628211ULL;
    assert(dsp_stereo::knownFunction(reinterpret_cast<void*>(mockOpen), 16, fingerprint));
    assert(!dsp_stereo::knownFunction(reinterpret_cast<void*>(mockOpen), 16, fingerprint ^ 1));
#endif
    reset(); openOwned(managerA, &ownerA, &handlerA, false); assert(!initSawStereo);
    reset(); openOwned(managerA, nullptr); assert(!initSawStereo);
    reset(); tetris_dsp_vendor_open(managerA.self(), &handlerA); assert(!initSawStereo);
    reset(); managerA.prepare(&handlerA, 2); assertStock();
    reset(); managerA.entry[1] = &handlerB; assertStock();
    reset(); managerA.entry[0] = &handlerB; assertStock();
    reset(); task = 31; assertStock();
    reset(); scenario = 11; assertStock();
    reset(); direction = 0; assertStock();
    reset(); outputDevice = 1; assertStock();
    reset(); secondOutput = 2; assertStock();
    reset(); platform = 1; assertStock();
    reset(); *managerA.managerSlot() = &objectA; assertStock();
    reset(); *managerA.configSlot() = &configA; assertStock();
    // Each packet/header guard is tested inside otherwise eligible fresh
    // creation, so rejection cannot be attributed to an earlier scope gate.
    for (size_t offset : {0u, 0x31cu, 0x6cu, 0x70u, 0x71u, 0x44u, 0x17cu, 0x24u, 0x28u}) {
        reset(); packetMutationOffset = offset; assertStock();
        assert(restoredPacket && rawCalls == 1);
    }
    reset(); packetBytes = 0x31f; assertStock();
    reset(); packetBytes = 0; assertStock();
    reset(); nullPayload = true; assertStock();
    for (int guard = 0; guard < 4; ++guard) {
        reset();
        if (guard == 0) transportLayer = 1;
        if (guard == 1) transportDataType = 1;
        if (guard == 2) transportAck = 0;
        if (guard == 3) transportParam2 = 1;
        assertStock();
        diagnostics = tetris_dsp_stereo_state();
        assert(diagnostics.initPackets == 1 && diagnostics.packetPlatform == 0 &&
                diagnostics.packetScenario == 0 && diagnostics.packetDirection == 0);
        assert(std::strcmp(diagnostics.lastGate, "packet_transport") == 0);
    }
    reset();
    auto invalid = packet();
    const std::array<unsigned char, 1> tiny{};
    assert(!dsp_stereo::packetMatches(nullptr, 0x320, 4));
    assert(!dsp_stereo::packetMatches(tiny.data(), tiny.size(), 4));
    assert(dsp_stereo::packetMatches(invalid.data(), invalid.size(), 4));
    assert(!dsp_stereo::packetMatches(invalid.data(), 0x31f, 4));
    write<uint32_t>(invalid.data(), 0x31c, 0);
    assert(!dsp_stereo::packetMatches(invalid.data(), invalid.size(), 4));

    // Failure after the packet was changed never forgets ownership.
    reset(); sendResult = -EIO; openOwned();
    status = tetris_dsp_stereo_state();
    assert(status.dirty && status.fault && !status.ready && status.initAccepted == 0 && restoredPacket);
    sendResult = 0;
    destroyOwned();
    status = tetris_dsp_stereo_state();
    assert(!status.dirty && status.fault); // Fault remains latched after cleanup.
    reset(); openResult = -EIO; assert(openOwned() == -EIO);
    assert(tetris_dsp_stereo_state().dirty && tetris_dsp_stereo_state().fault);
    assert(tetris_dsp_stereo_state().lastEligibleOpenResult == -EIO);
    reset(); returnObjects = false; openOwned();
    assert(tetris_dsp_stereo_state().dirty && tetris_dsp_stereo_state().fault);
    destroyOwned();
    assert(tetris_dsp_stereo_state().dirty); // No matching objects to acknowledge.

    for (int failure = 0; failure < 3; ++failure) {
        reset(); openOwned();
        if (failure == 0) sendResult = -EIO;
        if (failure == 1) sendDestroy = false;
        if (failure == 2) clearObjects = false;
        destroyOwned();
        status = tetris_dsp_stereo_state();
        assert(status.dirty && status.fault && !status.ready && status.teardowns == 0);
    }
    // Unrelated teardown cannot clear an owned task.
    reset(); openOwned();
    tetris_dsp_vendor_destroy(managerB.self(), managerB.managerSlot(), managerB.configSlot(), 32);
    assert(tetris_dsp_stereo_state().dirty);
    tetris_dsp_vendor_destroy(managerA.self(), managerA.managerSlot(), managerA.configSlot(), 31);
    assert(tetris_dsp_stereo_state().dirty);

    // Unexpected replacement/teardown stays stock and invalidates readiness.
    reset(); openOwned();
    auto recovery = packet();
    const auto recoveryBefore = recovery;
    tetris_dsp_vendor_send(nullptr, 32, 2, 2, 1, 0x15, recovery.size(), 0, recovery.data());
    status = tetris_dsp_stereo_state();
    assert(recovery == recoveryBefore && status.dirty && status.fault && !status.ready);

    // A nested ineligible write cannot borrow its outer scope.
    reset();
    TetrisDspStereoScope outer, inner;
    tetris_dsp_stereo_begin(&outer, &ownerA, true);
    tetris_dsp_stereo_begin(&inner, &ownerB, false);
    tetris_dsp_vendor_open(managerB.self(), &handlerB);
    assert(!initSawStereo);
    tetris_dsp_stereo_end(&inner);
    tetris_dsp_vendor_open(managerA.self(), &handlerA);
    assert(initSawStereo && tetris_dsp_stereo_state().owner == &ownerA);
    tetris_dsp_stereo_end(&outer);
    assert(!dsp_stereo::writeScope && !dsp_stereo::openScope && !dsp_stereo::createScope);

    // TLS admission does not leak to another thread.
    reset();
    tetris_dsp_stereo_begin(&outer, &ownerA, true);
    std::thread foreign([] { tetris_dsp_vendor_open(managerB.self(), &handlerB); });
    foreign.join();
    assert(!initSawStereo && !tetris_dsp_stereo_state().dirty);
    tetris_dsp_stereo_end(&outer);

    // Revocation before send defeats a previously eligible pending creation;
    // revocation during send retains dirty ownership and prevents readiness.
    for (bool before : {true, false}) {
        reset();
        pauseBeforeSend = before;
        pauseInsideSend = !before;
        std::thread pending([] { openOwned(); });
        {
            std::unique_lock lock(pauseMutex);
            pauseCv.wait(lock, [] { return entered; });
        }
        if (!before) assert(tetris_dsp_stereo_state().dirty);
        tetris_dsp_stereo_revoke();
        {
            std::lock_guard lock(pauseMutex);
            resume = true;
        }
        pauseCv.notify_all();
        pending.join();
        status = tetris_dsp_stereo_state();
        assert(!status.ready && restoredPacket);
        assert(status.dirty == !before && initSawStereo == !before);
        assert(status.eligibleWrites == 1 && status.eligibleOpens == 1 && status.freshCreates == 1 && status.initPackets == 1);
        assert(std::strcmp(status.lastGate, before ? "packet_generation" : "open_generation") == 0);
        if (!before) { assert(status.fault); destroyOwned(); assert(!tetris_dsp_stereo_state().dirty); }
    }
    reset();
    tetris_dsp_stereo_fault();
    assertStock();
    puts("DSP packet scope, stock gates, restoration, lifecycle failures, nesting and concurrency checks passed");
}
