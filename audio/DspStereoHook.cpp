// SPDX-License-Identifier: Apache-2.0
#include "DspStereoHook.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>

namespace dsp_stereo {
using Open = int (*)(void*, void*);
using Create = void (*)(void*, void**, void**, uint8_t, uint32_t, uint8_t, uint32_t,
                       const void*, const void*, const void*, const void*);
using Destroy = void (*)(void*, void**, void**, uint8_t);
using Send = int (*)(void*, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t,
                     uint32_t, uint32_t, void*);
constexpr char kOpen[] = "_ZN7android21AudioDspStreamManager16openPlaybackTaskEPNS_28AudioALSAPlaybackHandlerBaseE";
constexpr char kCreate[] = "_ZN7android21AudioDspStreamManager23CreateAurisysLibManagerEPP21aurisys_lib_manager_tPP20aurisys_dsp_config_thjhjPK18stream_attribute_tS9_S9_PK21aurisys_gain_config_t";
constexpr char kDestroy[] = "_ZN7android21AudioDspStreamManager24DestroyAurisysLibManagerEPP21aurisys_lib_manager_tPP20aurisys_dsp_config_th";
constexpr char kSend[] = "audio_send_ipi_msg";
struct Fingerprint { const char* symbol; size_t bytes; uint64_t hash; };
// Entire exported functions in the audited Tetris HAL. These cover packet
// construction/lifetime, the shared task and its locked SortedVector layout.
constexpr std::array<Fingerprint, 9> kFingerprints{{
    {kOpen, 0x1084, 0xde5cdbeb1f3e2cb9ULL},
    {kCreate, 0xda8, 0xa0ac94713d62aea7ULL},
    {kDestroy, 0x308, 0xcf2a0b2bbeb71487ULL},
    {kSend, 0x6d4, 0x3c576de5488d0dd2ULL},
    {"_ZN7android21AudioDspStreamManager17SetArsiTaskConfigEP28aurisys_lib_manager_config_thjjPK18stream_attribute_tS5_", 0x4d0, 0x4affa4e71e2855a3ULL},
    {"_ZN7android21AudioDspStreamManager17closePlaybackTaskEPNS_28AudioALSAPlaybackHandlerBaseE", 0x2d4, 0xf4b42edc1ff9e34bULL},
    {"_ZN7android21AudioDspStreamManager18getDspStreamStatusEv", 0x1e8, 0xd7dbc693f6860000ULL},
    {"_ZN7android21AudioDspStreamManager18addPlaybackHandlerEPNS_28AudioALSAPlaybackHandlerBaseE", 0x708, 0x2bf9ac41e57b6536ULL},
    {"_ZN7android21AudioDspStreamManager21removePlaybackHandlerEPNS_28AudioALSAPlaybackHandlerBaseE", 0xca8, 0xd75ba172d7272353ULL},
}};
std::atomic<Open> originalOpen{nullptr};
std::atomic<Create> originalCreate{nullptr};
std::atomic<Destroy> originalDestroy{nullptr};
std::atomic<Send> originalSend{nullptr};
std::atomic<bool> compatible{false};
std::mutex stateMutex;
struct State {
    bool dirty = false, ready = false, fault = false;
    void* owner = nullptr;
    void* handler = nullptr;
    void* manager = nullptr;
    void** managerSlot = nullptr;
    void** configSlot = nullptr;
    void* managerObject = nullptr;
    void* configObject = nullptr;
    uint64_t generation = 1;
    uint64_t taskGeneration = 0;
    uint64_t initAttempts = 0, initAccepted = 0, teardowns = 0;
    uint64_t eligibleWrites = 0, observedOpens = 0, eligibleOpens = 0;
    uint64_t freshCreates = 0, initPackets = 0;
    const char* lastGate = "unseen";
    uint32_t packetPlatform = 0, packetScenario = 0, packetDirection = 0;
    uint64_t handlerCount = 0;
    int32_t lastEligibleOpenResult = INT32_MIN;
} state;
thread_local TetrisDspStereoScope* writeScope = nullptr;
struct OpenScope {
    OpenScope* previous;
    void* manager;
    void* handler;
    void* owner;
    uint64_t generation;
    bool eligible = false;
    bool created = false;
    unsigned attempts = 0;
};
struct CreateScope {
    CreateScope* previous;
    OpenScope* opening;
    void** managerSlot;
    void** configSlot;
    uint32_t scenario;
    bool eligible;
};
struct DestroyScope {
    DestroyScope* previous;
    uint64_t taskGeneration;
    unsigned sends = 0;
    bool accepted = false;
};
thread_local OpenScope* openScope = nullptr;
thread_local CreateScope* createScope = nullptr;
thread_local DestroyScope* destroyScope = nullptr;
template <typename T> struct RestoreTls {
    T*& slot;
    T* previous;
    ~RestoreTls() { slot = previous; }
};
template <typename T> T read(const void* pointer, size_t offset = 0) {
    T result;
    std::memcpy(&result, static_cast<const unsigned char*>(pointer) + offset, sizeof(result));
    return result;
}
template <typename T> void write(void* pointer, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(pointer) + offset, &value, sizeof(value));
}
struct Mapping { uintptr_t address; size_t bytes; bool readable = false; };
int findMapping(dl_phdr_info* info, size_t, void* opaque) {
    auto& mapping = *static_cast<Mapping*>(opaque);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || !(phdr.p_flags & PF_R) || !(phdr.p_flags & PF_X)) continue;
        const uintptr_t begin = info->dlpi_addr + phdr.p_vaddr;
        if (mapping.address >= begin && mapping.address - begin <= phdr.p_memsz &&
                mapping.bytes <= phdr.p_memsz - (mapping.address - begin)) {
            mapping.readable = true;
            return 1;
        }
    }
    return 0;
}
bool knownFunction(void* function, size_t bytes, uint64_t fingerprint) {
    if (!function) return false;
    Mapping mapping{reinterpret_cast<uintptr_t>(function), bytes};
    dl_iterate_phdr(findMapping, &mapping);
#if defined(__aarch64__)
    if (mapping.readable) {
        uint64_t hash = 14695981039346656037ULL;
        const auto* code = static_cast<const uint8_t*>(function);
        for (size_t i = 0; i < bytes; ++i) hash = (hash ^ code[i]) * 1099511628211ULL;
        return hash == fingerprint;
    }
#else
    (void)fingerprint;
#endif
    return false;
}
bool soleHandler(void* manager, void* handler) {
    if (!manager || !handler || read<size_t>(manager, 0x270) != 1) return false;
    const auto* entries = read<const void*>(manager, 0x268);
    return entries && read<void*>(entries) == handler && read<void*>(entries, 8) == handler;
}
bool sharedSlots(void* manager, void** managerSlot, void** configSlot) {
    auto* base = static_cast<unsigned char*>(manager);
    return manager && managerSlot == reinterpret_cast<void**>(base + 0xbd8) &&
            configSlot == reinterpret_cast<void**>(base + 0xbe0);
}
bool playbackScenario(uint32_t scenario) {
    // The audited open selects normal playback (4) or SmartPA playback (10).
    // Keep the vendor's choice so its speaker protection remains enabled.
    return scenario == 4 || scenario == 10;
}
bool packetMatches(const void* data, uint32_t bytes, uint32_t expectedScenario) {
    return playbackScenario(expectedScenario) && data && bytes >= 0x320 &&
            read<uint32_t>(data) == 0x55667788 && read<uint32_t>(data, 0x31c) == 0xaabbccdd &&
            read<uint32_t>(data, 0x6c) == expectedScenario && read<uint8_t>(data, 0x70) == 2 &&
            read<uint8_t>(data, 0x71) == 1 &&
            read<uint8_t>(data, 0x44) == 32 && read<uint8_t>(data, 0x17c) == 32 &&
            read<uint32_t>(data, 0x24) == 2 && read<uint32_t>(data, 0x28) == 0;
}
void latchFault() {
    state.fault = true;
    state.ready = false;
    ++state.generation;
}
} // namespace dsp_stereo

extern "C" int tetris_dsp_vendor_open(void*, void*)
        asm("_ZN7android21AudioDspStreamManager16openPlaybackTaskEPNS_28AudioALSAPlaybackHandlerBaseE");
extern "C" void tetris_dsp_vendor_create(void*, void**, void**, uint8_t, uint32_t, uint8_t,
        uint32_t, const void*, const void*, const void*, const void*)
        asm("_ZN7android21AudioDspStreamManager23CreateAurisysLibManagerEPP21aurisys_lib_manager_tPP20aurisys_dsp_config_thjhjPK18stream_attribute_tS9_S9_PK21aurisys_gain_config_t");
extern "C" void tetris_dsp_vendor_destroy(void*, void**, void**, uint8_t)
        asm("_ZN7android21AudioDspStreamManager24DestroyAurisysLibManagerEPP21aurisys_lib_manager_tPP20aurisys_dsp_config_th");
extern "C" int tetris_dsp_vendor_send(void*, uint8_t, uint8_t, uint8_t, uint8_t,
        uint16_t, uint32_t, uint32_t, void*) asm("audio_send_ipi_msg");

extern "C" int tetris_dsp_vendor_open(void* manager, void* handler) {
    using namespace dsp_stereo;
    auto original = originalOpen.load(std::memory_order_acquire);
    if (!original) return -ENODEV;
    OpenScope current{openScope, manager, handler, nullptr, 0};
    {
        std::lock_guard lock(stateMutex);
        ++state.observedOpens;
        state.lastGate = compatible.load(std::memory_order_acquire) ? "open_scope" : "open_incompatible";
    }
    if (!openScope && writeScope && writeScope->eligible && writeScope->owner &&
            compatible.load(std::memory_order_acquire)) {
        std::lock_guard lock(stateMutex);
        current.owner = writeScope->owner;
        current.generation = writeScope->generation;
        if (current.generation == state.generation && !state.dirty && !state.fault && manager && handler) {
            state.handlerCount = read<size_t>(manager, 0x270);
        }
        current.eligible = current.generation == state.generation && !state.dirty && !state.fault &&
                soleHandler(manager, handler);
        state.lastGate = current.eligible ? "open_eligible" :
                current.generation != state.generation ? "open_generation" :
                state.dirty ? "open_dirty" : state.fault ? "open_fault" : "open_handler";
        if (current.eligible) ++state.eligibleOpens;
    }
    RestoreTls restore{openScope, openScope};
    openScope = &current;
    const int result = original(manager, handler);
    if (current.eligible) {
        std::lock_guard lock(stateMutex);
        state.lastEligibleOpenResult = result;
    }
    if (current.attempts) {
        std::lock_guard lock(stateMutex);
        if (state.dirty && state.taskGeneration == current.generation && state.owner == current.owner) {
            if (result == 0 && current.created && current.attempts == 1 && !state.fault &&
                    state.generation == current.generation) {
                state.ready = true;
                state.lastGate = "ready";
            } else {
                if (!state.fault) state.lastGate = state.generation != current.generation ? "open_generation" :
                        result != 0 ? "open_result" : "open_incomplete";
                latchFault();
            }
        }
    }
    return result;
}

extern "C" void tetris_dsp_vendor_create(void* manager, void** managerSlot, void** configSlot,
        uint8_t task, uint32_t scenario, uint8_t direction, uint32_t flags,
        const void* input, const void* output, const void* echo, const void* gain) {
    using namespace dsp_stereo;
    auto original = originalCreate.load(std::memory_order_acquire);
    if (!original) return;
    const bool eligible = !createScope && openScope && openScope->eligible &&
            openScope->manager == manager && task == 32 && playbackScenario(scenario) && direction == 1 &&
            input && output && sharedSlots(manager, managerSlot, configSlot) &&
            !read<void*>(managerSlot) && !read<void*>(configSlot);
    if (openScope && openScope->eligible) {
        std::lock_guard lock(stateMutex);
        state.lastGate = eligible ? "create_fresh" : createScope ? "create_nested" :
                openScope->manager != manager ? "create_manager" : task != 32 ? "create_task" :
                !playbackScenario(scenario) ? "create_scenario" : direction != 1 ? "create_direction" :
                !input || !output ? "create_attributes" :
                !sharedSlots(manager, managerSlot, configSlot) ? "create_slots" : "create_existing";
        if (eligible) ++state.freshCreates;
    }
    CreateScope current{createScope, openScope, managerSlot, configSlot, scenario, eligible};
    RestoreTls restore{createScope, createScope};
    createScope = &current;
    original(manager, managerSlot, configSlot, task, scenario, direction, flags, input, output, echo, gain);
    if (eligible && current.opening->attempts) {
        std::lock_guard lock(stateMutex);
        if (state.dirty && state.taskGeneration == current.opening->generation && state.manager == manager) {
            // Failed allocation paths may free the temporary config: inspect
            // only the audited output slots, never a retained temporary pointer.
            state.managerObject = read<void*>(managerSlot);
            state.configObject = read<void*>(configSlot);
            current.opening->created = state.managerObject && state.configObject;
            if (!current.opening->created) latchFault();
        }
    }
}

extern "C" int tetris_dsp_vendor_send(void* message, uint8_t task, uint8_t layer,
        uint8_t dataType, uint8_t ackType, uint16_t messageId,
        uint32_t param1, uint32_t param2, void* data) {
    using namespace dsp_stereo;
    auto original = originalSend.load(std::memory_order_acquire);
    if (!original) return -ENODEV;
    if (destroyScope && task == 32 && layer == 2 && dataType == 0 && ackType == 1 &&
            messageId == 0x16 && !param1 && !param2 && !data) {
        ++destroyScope->sends;
        const int result = original(message, task, layer, dataType, ackType, messageId, param1, param2, data);
        destroyScope->accepted = destroyScope->sends == 1 && result == 0;
        return result;
    }
    bool patch = false;
    OpenScope* opening = createScope ? createScope->opening : nullptr;
    const bool observedInit = createScope && createScope->eligible && opening && opening == openScope &&
            task == 32 && messageId == 0x15;
    if (observedInit) {
        std::lock_guard lock(stateMutex);
        ++state.initPackets;
        state.packetPlatform = state.packetScenario = state.packetDirection = 0;
        // Read only the audited INIT transport and a complete packet header.
        // Other payload types may not use param1 as an allocation size.
        if (layer == 2 && dataType == 2 && ackType == 1 && param2 == 0 &&
                data && param1 >= 0x320) {
            state.packetPlatform = read<uint8_t>(data, 0x70);
            state.packetScenario = read<uint32_t>(data, 0x6c);
            state.packetDirection = read<uint8_t>(data, 0x71);
        }
        state.lastGate = !writeScope || !writeScope->eligible || writeScope->owner != opening->owner ||
                writeScope->generation != opening->generation ? "packet_scope" :
                layer != 2 || dataType != 2 || ackType != 1 || param2 != 0 ? "packet_transport" : "packet_layout";
    }
    if (createScope && createScope->eligible && opening && opening == openScope &&
            writeScope && writeScope->eligible && writeScope->owner == opening->owner &&
            writeScope->generation == opening->generation &&
            task == 32 && layer == 2 && dataType == 2 && ackType == 1 && messageId == 0x15 &&
            param2 == 0 && packetMatches(data, param1, createScope->scenario)) {
        std::lock_guard lock(stateMutex);
        if (compatible.load(std::memory_order_acquire) && !state.dirty && !state.fault &&
                state.generation == opening->generation && soleHandler(opening->manager, opening->handler)) {
            // An error return cannot prove that firmware saw no change.
            state.dirty = true;
            state.ready = false;
            state.owner = opening->owner;
            state.handler = opening->handler;
            state.manager = opening->manager;
            state.managerSlot = createScope->managerSlot;
            state.configSlot = createScope->configSlot;
            state.taskGeneration = opening->generation;
            ++state.initAttempts;
            ++opening->attempts;
            state.lastGate = "init_sent";
            patch = true;
        } else if (observedInit) {
            state.lastGate = !compatible.load(std::memory_order_acquire) ? "packet_incompatible" :
                    state.generation != opening->generation ? "packet_generation" :
                    state.dirty ? "packet_dirty" : state.fault ? "packet_fault" : "packet_handler";
        }
    }
    if (!patch) {
        if (task == 32 && layer == 2 && (messageId == 0x15 || messageId == 0x16)) {
            std::lock_guard lock(stateMutex);
            // Recovery or another unrecognized lifecycle path may replace the
            // task. Forward it unchanged, but never keep claiming readiness.
            if (state.dirty) latchFault();
        }
        return original(message, task, layer, dataType, ackType, messageId, param1, param2, data);
    }
    const uint16_t saved = read<uint16_t>(data, 0x42);
    write<uint16_t>(data, 0x42, saved | 0x4);
    struct RestorePacket {
        void* data;
        uint16_t saved;
        ~RestorePacket() { write<uint16_t>(data, 0x42, saved); }
    } restore{data, saved};
    const int result = original(message, task, layer, dataType, ackType, messageId, param1, param2, data);
    {
        std::lock_guard lock(stateMutex);
        if (result == 0) {
            ++state.initAccepted;
            state.lastGate = "init_accepted";
        } else {
            state.lastGate = "init_error";
            latchFault();
        }
    }
    return result;
}

extern "C" void tetris_dsp_vendor_destroy(void* manager, void** managerSlot, void** configSlot,
                                          uint8_t task) {
    using namespace dsp_stereo;
    auto original = originalDestroy.load(std::memory_order_acquire);
    if (!original) return;
    DestroyScope current{destroyScope, 0};
    {
        std::lock_guard lock(stateMutex);
        if (!destroyScope && state.dirty && task == 32 && manager == state.manager &&
                managerSlot == state.managerSlot && configSlot == state.configSlot &&
                state.managerObject && state.configObject &&
                read<void*>(managerSlot) == state.managerObject && read<void*>(configSlot) == state.configObject) {
            current.taskGeneration = state.taskGeneration;
            state.ready = false;
        }
    }
    RestoreTls restore{destroyScope, destroyScope};
    destroyScope = current.taskGeneration ? &current : nullptr;
    original(manager, managerSlot, configSlot, task);
    if (current.taskGeneration) {
        std::lock_guard lock(stateMutex);
        if (state.dirty && state.taskGeneration == current.taskGeneration) {
            if (current.sends == 1 && current.accepted && !read<void*>(managerSlot) && !read<void*>(configSlot)) {
                state.dirty = state.ready = false;
                state.owner = state.handler = state.manager = nullptr;
                state.managerSlot = state.configSlot = nullptr;
                state.managerObject = state.configObject = nullptr;
                ++state.teardowns;
                ++state.generation;
            } else latchFault();
        }
    }
}

extern "C" bool tetris_dsp_stereo_initialize(void* vendorHandle) {
    using namespace dsp_stereo;
    compatible.store(false, std::memory_order_release);
    auto opening = reinterpret_cast<Open>(dlsym(vendorHandle, kOpen));
    auto creating = reinterpret_cast<Create>(dlsym(vendorHandle, kCreate));
    auto destroying = reinterpret_cast<Destroy>(dlsym(vendorHandle, kDestroy));
    auto sending = reinterpret_cast<Send>(dlsym(vendorHandle, kSend));
    if (opening == tetris_dsp_vendor_open) opening = nullptr;
    if (creating == tetris_dsp_vendor_create) creating = nullptr;
    if (destroying == tetris_dsp_vendor_destroy) destroying = nullptr;
    if (sending == tetris_dsp_vendor_send) sending = nullptr;
    // Resolve every forwarding path independently: a missing or changed symbol
    // disables the experiment without breaking the other stock callbacks.
    originalOpen.store(opening, std::memory_order_release);
    originalCreate.store(creating, std::memory_order_release);
    originalDestroy.store(destroying, std::memory_order_release);
    originalSend.store(sending, std::memory_order_release);
    bool known = opening && creating && destroying && sending;
    for (const auto& item : kFingerprints) {
        known &= knownFunction(dlsym(vendorHandle, item.symbol), item.bytes, item.hash);
    }
    compatible.store(known, std::memory_order_release);
    return known;
}
extern "C" bool tetris_dsp_stereo_compatible() {
    return dsp_stereo::compatible.load(std::memory_order_acquire);
}
extern "C" void tetris_dsp_stereo_begin(TetrisDspStereoScope* current, void* owner, bool eligible) {
    using namespace dsp_stereo;
    std::lock_guard lock(stateMutex);
    *current = {writeScope, owner, state.generation, eligible && owner && !state.fault};
    if (current->eligible) {
        ++state.eligibleWrites;
        state.lastGate = "write_eligible";
    }
    writeScope = current;
}
extern "C" void tetris_dsp_stereo_end(TetrisDspStereoScope* current) {
    dsp_stereo::writeScope = current->previous;
}
extern "C" void tetris_dsp_stereo_revoke() {
    using namespace dsp_stereo;
    std::lock_guard lock(stateMutex);
    ++state.generation;
    state.ready = false;
}
extern "C" void tetris_dsp_stereo_fault() {
    std::lock_guard lock(dsp_stereo::stateMutex);
    dsp_stereo::latchFault();
}
extern "C" TetrisDspStereoState tetris_dsp_stereo_state() {
    using namespace dsp_stereo;
    std::lock_guard lock(stateMutex);
    return {compatible.load(std::memory_order_acquire), state.dirty, state.ready, state.fault,
            state.owner, state.handler, state.initAttempts, state.initAccepted, state.teardowns,
            state.eligibleWrites, state.observedOpens, state.eligibleOpens, state.freshCreates,
            state.initPackets, state.lastGate, state.packetPlatform, state.packetScenario,
            state.packetDirection, state.handlerCount, state.lastEligibleOpenResult};
}
