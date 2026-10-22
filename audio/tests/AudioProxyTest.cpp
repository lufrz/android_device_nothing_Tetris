// SPDX-License-Identifier: Apache-2.0
// Host-only callbacks and ioctl mocks. Never opens a mixer or Android device.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../LegacyAudioHal.h"
#define LOG_TAG "TetrisAudio"
#include <log/log.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <array>
#include "../DownmixHook.h"
#include "../DspStereoHook.h"
#include "../TetrisAudioAbi.h"

#include "../AudioProxy.cpp"

static bool acceptedConsent = true;
static std::string authorizedMode = "off";
static std::string authorizedGain = "0";
static std::string authorizedSoftwareGain = "0", authorizedSyncTrim = "0", authorizedOutput = "both";
static int appliedSoftwareGain;
static unsigned softwareGainUpdates;
static tetris::audio::SpeakerOutput lastSplitOutput = tetris::audio::SpeakerOutput::Both;
namespace android::base {
std::string GetProperty(const std::string& key, const std::string&) {
    if (key == "sys.tetrisparts.consent") return acceptedConsent ? "1" : "0";
    if (key == "persist.sys.tetris.audio_mode") return authorizedMode;
    if (key == "persist.sys.tetris.audio_gain") return authorizedGain;
    if (key == "persist.sys.tetris.audio_software_gain") return authorizedSoftwareGain;
    if (key == "persist.sys.tetris.audio_sync_trim") return authorizedSyncTrim;
    assert(key == "persist.sys.tetris.audio_output");
    return authorizedOutput;
}
}
static unsigned kernelState;
static int kernelStateReadError;
static unsigned kernelGain;
static int kernelGainDb;
static int gainSetError;
static bool gainSupported;
static bool revokeDuringGain;
static unsigned gainUpdates;
static bool hookKnown;
static bool hookReady = true;
static bool dspKnown;
static unsigned dspMutations;
static int modeResult;
static bool bypass;
static uint64_t hookCalls;
static uint64_t mockGeneration;
static TetrisHookScope* mockScope;
static int handlerA;
static void* fakeHandler = &handlerA;
static bool ambiguousWrite = false;
static bool observeHook = true;
static bool rejectAcquire;
static int acquireError = EBUSY;
static unsigned leaseAttempts;
static bool kernelExists;
static int parameterResult;
static bool revokeDuringParameter;
static unsigned parameterKernelState;
static std::string forwardedParameters;
static const audio_port_config* forwardedPort;
static ssize_t writeResult;
static int standbyResult;
static unsigned standbyCalls;
static void (*writeReentry)();
static void (*standbyReentry)();
static int startResult;
static int resumeResult;
static int stopResult;
static bool withLifecycleCallbacks;
static void (*activationReentry)();
static bool wideDiagnosticCounters;
static unsigned acquisitions;
static bool lastBypass;
static std::vector<std::string> events;
static unsigned patchSourceCount;
static int patchResult, releasePatchResult;
static audio_patch_handle_t nextPatchHandle;
static unsigned patchSinkCount;
static const audio_port_config* patchSources;
static const audio_port_config* patchSinks;
static std::mutex blockMutex;
static std::condition_variable blockCv;
static bool blockWrite = false;
static bool blockEntered = false;
static bool releaseBlockedWrite = false;
static audio_stream_out* blockedTarget = nullptr;

// These mocks isolate proxy routing/accounting from ALSA and the receiver worker.
// Split samples/clock validation are exercised with production helpers separately.
static bool splitSupported = true;
static uint32_t currentRate = 48000;
static int positionResult;
static unsigned positionQueries;
static unsigned invalidPositionQuery;
static bool invalidStamp;
static bool targetValid = true;
static bool splitSucceeds = true;
static bool receiverStartFails, receiverPrepareFails, receiverEnqueueFails;
static int receiverPrepareError = EINVAL;
static int receiverAsyncError;
static uint64_t receiverUnwrittenFrames;
static unsigned receiverStarts, receiverPrepares, receiverCloses, receiverEnqueues;
static bool traceReceiverLifecycle;
static std::vector<std::string> receiverLifecycle;
static std::vector<int32_t> receiverSamples;
static uint64_t receiverFirstFrame;
static int64_t receiverDeadline;
static int64_t calculatedDeadline;
static int64_t calculatedDelayNs;
static bool exactWriteResult;
static const void* vendorBuffer;
static std::vector<uint8_t> vendorBytes;
static std::map<const audio_stream_out*, uint64_t> vendorFrames;
namespace tetris::audio {
struct StereoReceiver::Impl { ReceiverState state; bool prepared = false; };
StereoReceiver::StereoReceiver() : impl_(std::make_unique<Impl>()) {}
StereoReceiver::~StereoReceiver() = default;
bool StereoReceiver::start(int fd) {
    assert(fd == 123 && kernelState == 0);
    ++receiverStarts;
    if (traceReceiverLifecycle) receiverLifecycle.emplace_back("start");
    impl_->state = {};
    impl_->prepared = false;
    receiverAsyncError = 0; receiverUnwrittenFrames = 0;
    if (receiverStartFails) { impl_->state.failed = true; impl_->state.error = ENODEV; return false; }
    impl_->state.open = true;
    return true;
}
bool StereoReceiver::prepare() {
    assert(impl_->state.open && kernelState == 2 && !impl_->prepared);
    ++receiverPrepares;
    if (traceReceiverLifecycle) receiverLifecycle.emplace_back("prepare");
    if (receiverPrepareFails) {
        impl_->state.failed = true;
        impl_->state.error = receiverPrepareError;
        return false;
    }
    impl_->prepared = true;
    return true;
}
bool StereoReceiver::enqueue(std::vector<int32_t> samples, size_t acceptedFrames,
                             uint64_t firstFrame, int64_t targetNs) {
    assert(impl_->state.open && impl_->prepared && kernelState == 2);
    ++receiverEnqueues;
    assert(acceptedFrames <= samples.size());
    if (receiverEnqueueFails) { impl_->state.failed = true; impl_->state.error = EPIPE; return false; }
    receiverSamples.assign(samples.begin(), samples.begin() + acceptedFrames);
    receiverFirstFrame = firstFrame;
    receiverDeadline = targetNs;
    impl_->state.running = true;
    impl_->state.queuedFrames += acceptedFrames;
    impl_->state.writtenFrames += acceptedFrames;
    return true;
}
void StereoReceiver::close() {
    if (impl_->state.open) {
        ++receiverCloses;
        if (traceReceiverLifecycle) receiverLifecycle.emplace_back("receiver_close");
    }
    impl_->state.open = impl_->state.running = false;
    impl_->prepared = false;
}
ReceiverState StereoReceiver::state() const {
    auto result = impl_->state;
    if (wideDiagnosticCounters) {
        result.queuedFrames = result.writtenFrames = result.playedFrames = result.underruns = UINT64_MAX;
        result.clockErrorNs = INT64_MIN;
    }
    if (result.open && receiverAsyncError) {
        assert(receiverUnwrittenFrames <= result.writtenFrames);
        result.failed = true;
        result.running = false;
        result.error = receiverAsyncError;
        result.writtenFrames -= receiverUnwrittenFrames;
    }
    return result;
}
bool StereoReceiver::setSoftwareGain(int gain) {
    if (gain < 0 || gain > 6) return false;
    appliedSoftwareGain = gain;
    ++softwareGainUpdates;
    return true;
}
bool splitStereo(const void* input, size_t bytes, unsigned bits, StereoBlock* result,
                 SpeakerOutput output) {
    lastSplitOutput = output;
    if (!splitSucceeds || !input || !bytes || (bits != 16 && bits != 32) ||
            bytes % (bits / 4)) return false;
    result->frameBytes = bits / 4;
    result->right.assign(bytes, 0);
    result->left.clear();
    const auto* pcm = static_cast<const uint8_t*>(input);
    for (size_t offset = 0; offset < bytes; offset += result->frameBytes) {
        if (bits == 16) {
            int16_t left; std::memcpy(&left, pcm + offset, sizeof(left));
            result->left.push_back(int32_t(left) * 65536);
        } else {
            int32_t left; std::memcpy(&left, pcm + offset, sizeof(left));
            result->left.push_back(left);
        }
        std::memcpy(result->right.data() + offset + bits / 8,
                    pcm + offset + bits / 8, bits / 8);
    }
    return true;
}
std::optional<int64_t> receiverTargetTime(PresentationClock clock, uint64_t firstFrame,
                                        int64_t nowNs, int64_t delayNs) {
    assert(nowNs > 0 && clock.timeNs > 0);
    if (!targetValid || clock.frames > firstFrame) return {};
    calculatedDelayNs = delayNs;
    calculatedDeadline = nowNs + 20000000 + delayNs;
    return calculatedDeadline; // Proxy must carry the helper's deadline unchanged.
}
}

extern "C" int __wrap_open(const char* path, int, ...) {
    assert(std::string(path) == "/dev/tetris_audio");
    if (!kernelExists) { errno = ENOENT; return -1; }
    kernelGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    return 123;
}
extern "C" int __wrap_close(int fd) {
    assert(fd == 123 && !receiver.state().open);
    if (traceReceiverLifecycle) receiverLifecycle.emplace_back("lease_close");
    if (kernelState) events.emplace_back("restore");
    kernelState = 0;
    kernelGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    return 0;
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert(fd == 123);
    va_list args;
    va_start(args, request);
    void* value = va_arg(args, void*);
    va_end(args);
    if (request == TETRIS_AUDIO_GET_CAPS) {
        *static_cast<tetris_audio_caps*>(value) = {1, (1u << 1) | (1u << 2), kernelGain, 0};
    } else if (request == TETRIS_AUDIO_GET_SPLIT_CAPS) {
        if (!splitSupported) { errno = ENOTTY; return -1; }
        *static_cast<tetris_audio_split_caps*>(value) = {
            TETRIS_AUDIO_SPLIT_VERSION, 0, 4, 48000, 1,
            TETRIS_AUDIO_SPLIT_FORMAT_S32_LE, {0, 0}};
    } else if (request == TETRIS_AUDIO_GET_STATE) {
        if (kernelStateReadError) { errno = kernelStateReadError; return -1; }
        *static_cast<__u32*>(value) = kernelState;
    } else if (request == TETRIS_AUDIO_SET_MODE) {
        ++leaseAttempts;
        if (rejectAcquire) {
            if (traceReceiverLifecycle) receiverLifecycle.emplace_back("route_failed");
            errno = acquireError;
            return -1;
        }
        kernelState = *static_cast<__u32*>(value);
        if (traceReceiverLifecycle) receiverLifecycle.emplace_back("route_on");
        ++acquisitions;
        events.emplace_back("acquire");
    } else if (request == TETRIS_AUDIO_GET_GAIN) {
        if (!gainSupported) { errno = ENOTTY; return -1; }
        *static_cast<__s32*>(value) = kernelGainDb;
    } else if (request == TETRIS_AUDIO_SET_GAIN) {
        if (!gainSupported) { errno = ENOTTY; return -1; }
        if (gainSetError) { errno = gainSetError; return -1; }
        const int gain = *static_cast<__s32*>(value);
        assert(gain >= TETRIS_AUDIO_RECEIVER_MIN_DB && gain <= TETRIS_AUDIO_RECEIVER_MAX_DB);
        kernelGainDb = gain;
        ++gainUpdates;
        if (revokeDuringGain) kernelState = 0;
    } else assert(false);
    return 0;
}
extern "C" bool tetris_audio_hook_initialize(void*) { return hookReady; }
extern "C" TetrisBessoundProbeStats tetris_bessound_probe_stats() {
    if (!wideDiagnosticCounters) return {};
    constexpr auto maximum = UINT64_MAX;
    return {true, maximum, maximum, maximum, maximum, maximum, maximum,
            maximum, maximum, maximum, maximum, true, true, true};
}
extern "C" bool tetris_audio_hook_compatible() { return hookKnown; }
extern "C" uint64_t tetris_audio_hook_bind(void* handler) {
    assert(handler && hookKnown);
    return ++mockGeneration;
}
extern "C" void tetris_audio_hook_clear() { ++mockGeneration; bypass = false; }
extern "C" void tetris_audio_hook_begin(TetrisHookScope* current, void* handler, uint64_t generation) {
    *current = {};
    current->previous = mockScope;
    current->handler = handler;
    current->generation = generation;
    mockScope = current;
    bypass = generation && generation == mockGeneration && handler == fakeHandler;
}
extern "C" void tetris_audio_hook_end(TetrisHookScope* current) {
    mockScope = current->previous;
    bypass = mockScope && mockScope->generation == mockGeneration;
}
extern "C" TetrisHookStats tetris_audio_hook_stats() {
    if (wideDiagnosticCounters) {
        constexpr auto maximum = UINT64_MAX;
        return {maximum, maximum, maximum, maximum, maximum, maximum, maximum, maximum};
    }
    TetrisHookStats stats{};
    stats.original = hookCalls;
    return stats;
}
extern "C" bool tetris_dsp_stereo_initialize(void*) { return dspKnown; }
extern "C" bool tetris_dsp_stereo_compatible() { return dspKnown; }
// Legacy DSP metadata mutation must never run in the receiver split path.
extern "C" void tetris_dsp_stereo_begin(TetrisDspStereoScope*, void*, bool) {
    ++dspMutations; assert(false);
}
extern "C" void tetris_dsp_stereo_end(TetrisDspStereoScope*) {
    ++dspMutations; assert(false);
}
extern "C" void tetris_dsp_stereo_revoke() {}
extern "C" void tetris_dsp_stereo_fault() { ++dspMutations; assert(false); }
extern "C" TetrisDspStereoState tetris_dsp_stereo_state() {
    TetrisDspStereoState state{};
    state.lastGate = "unseen";
    return state;
}
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }

static audio_format_t currentFormat = AUDIO_FORMAT_PCM_16_BIT;
static audio_format_t format(const audio_stream*) { return currentFormat; }
static uint32_t sampleRate(const audio_stream*) { return currentRate; }
static int presentationPosition(const audio_stream_out* target, uint64_t* frames, timespec* stamp) {
    ++positionQueries;
    *frames = vendorFrames[target];
    clock_gettime(CLOCK_MONOTONIC, stamp);
    if (invalidStamp || positionQueries == invalidPositionQuery) stamp->tv_nsec = 1000000000;
    return positionResult;
}
static audio_channel_mask_t channels(const audio_stream*) { return AUDIO_CHANNEL_OUT_STEREO; }
static ssize_t fakeWrite(audio_stream_out* target, const void* data, size_t bytes) {
    ++hookCalls;
    if (writeReentry) {
        auto callback = writeReentry;
        writeReentry = nullptr;
        callback();
    }
    if (mockScope && observeHook) {
        mockScope->observedHandler = fakeHandler;
        mockScope->ambiguous = ambiguousWrite;
    }
    lastBypass = bypass;
    events.emplace_back("write");
    if (traceReceiverLifecycle) receiverLifecycle.emplace_back("vendor_write");
    if (blockWrite && target == blockedTarget) {
        std::unique_lock lock(blockMutex);
        blockEntered = true;
        blockCv.notify_all();
        blockCv.wait(lock, [] { return releaseBlockedWrite; });
    }
    vendorBuffer = data;
    vendorBytes.clear();
    if (data && data != reinterpret_cast<const void*>(1) && bytes)
        vendorBytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    const ssize_t accepted = exactWriteResult ? writeResult :
            writeResult > 0 ? static_cast<ssize_t>(bytes) : writeResult;
    const size_t frameBytes = currentFormat == AUDIO_FORMAT_PCM_32_BIT ? 8 : 4;
    if (accepted > 0 && size_t(accepted) <= bytes && size_t(accepted) % frameBytes == 0)
        vendorFrames[target] += size_t(accepted) / frameBytes;
    return accepted;
}
static int fakeStandby(audio_stream*) {
    ++standbyCalls;
    events.emplace_back("standby");
    if (standbyReentry) {
        auto callback = standbyReentry;
        standbyReentry = nullptr;
        callback();
    }
    return standbyResult;
}
static int fakeStart(const audio_stream_out*) {
    assert(!dspMutations);
    events.emplace_back("start");
    if (activationReentry) {
        auto callback = activationReentry;
        activationReentry = nullptr;
        callback();
    }
    return startResult;
}
static int fakeResume(audio_stream_out*) {
    assert(!dspMutations);
    events.emplace_back("resume");
    if (activationReentry) {
        auto callback = activationReentry;
        activationReentry = nullptr;
        callback();
    }
    return resumeResult;
}
static int fakeStop(const audio_stream_out*) {
    assert(!dspMutations);
    events.emplace_back("stop");
    return stopResult;
}
static int parameterCallback() {
    parameterKernelState = kernelState;
    if (revokeDuringParameter) kernelState = 0;
    return parameterResult;
}
static int fakeStreamParameters(audio_stream*, const char* parameters) {
    forwardedParameters = parameters ? parameters : "";
    events.emplace_back("stream_parameters"); return parameterCallback();
}
static int fakeSetMode(audio_hw_device*, audio_mode_t) { events.emplace_back("mode"); return modeResult; }
static int fakeParameters(audio_hw_device*, const char* parameters) {
    forwardedParameters = parameters ? parameters : "";
    return parameterCallback();
}
static int fakePort(audio_hw_device*, const audio_port_config* config) {
    forwardedPort = config;
    return parameterCallback();
}
static char* fakeGetParameters(const audio_hw_device*, const char*) { return strdup("vendor=ok"); }
static int fakePatch(audio_hw_device*, unsigned sourceCount, const audio_port_config* sources,
                     unsigned sinkCount, const audio_port_config* sinks,
                     audio_patch_handle_t* handle) {
    patchSourceCount = sourceCount; patchSources = sources;
    patchSinkCount = sinkCount; patchSinks = sinks;
    events.emplace_back("patch");
    if (patchResult == 0 && handle) *handle = nextPatchHandle;
    return patchResult;
}
static int fakeReleasePatch(audio_hw_device*, audio_patch_handle_t) { return releasePatchResult; }
static int fakeInput(audio_hw_device*, audio_io_handle_t, audio_devices_t, audio_config*,
                     audio_stream_in**, audio_input_flags_t, const char*, audio_source_t) {
    events.emplace_back("input"); return 0;
}
static void fakeCloseInput(audio_hw_device*, audio_stream_in*) {}
static void fakeMetadata(audio_stream_out*, const source_metadata*) {
    events.emplace_back("metadata");
}
static void fakeMetadataV7(audio_stream_out*, const source_metadata_v7*) {
    events.emplace_back("metadata_v7");
}

static audio_hw_device fakeDevice{};
static hw_module_methods_t fakeModuleMethods{};
static hw_module_t fakeModule{};
static audio_stream_out abiStream{};
static bool unsupportedStream = false;
static int fakeDeviceClose(hw_device_t*) { events.emplace_back("device_close"); return 0; }
static int fakeOpenDevice(const hw_module_t* module, const char*, hw_device_t** output) {
    assert(module == &fakeModule && module->dso == &fakeModule);
    *output = &fakeDevice.common;
    return 0;
}
static int fakeOpenOutput(audio_hw_device*, audio_io_handle_t, audio_devices_t,
                          audio_output_flags_t, audio_config*, audio_stream_out** output,
                          const char*) {
    abiStream = {};
    abiStream.write = unsupportedStream ? nullptr : fakeWrite;
    abiStream.common.standby = fakeStandby;
    abiStream.common.set_parameters = fakeStreamParameters;
    abiStream.common.get_format = format;
    abiStream.common.get_channels = channels;
    abiStream.common.get_sample_rate = sampleRate;
    abiStream.get_presentation_position = presentationPosition;
    if (withLifecycleCallbacks) {
        abiStream.start = fakeStart;
        abiStream.resume = fakeResume;
        abiStream.stop = fakeStop;
    }
    *output = &abiStream;
    return 0;
}
static void fakeCloseOutput(audio_hw_device*, audio_stream_out*) {}
extern "C" void* __wrap_dlopen(const char* path, int) {
    assert(std::string(path) == "/vendor/lib64/hw/audio.primary.mediatek.so");
    return &fakeModule;
}
extern "C" void* __real_dlsym(void*, const char*);
extern "C" __attribute__((no_sanitize("address", "undefined")))
void* __wrap_dlsym(void* handle, const char* symbol) {
    // Sanitizer runtimes resolve libc symbols before their instrumentation is
    // initialized; instrument the rest of this test, but not this lookup edge.
    if (handle != &fakeModule) return __real_dlsym(handle, symbol);
    assert(std::string(symbol) == HAL_MODULE_INFO_SYM_AS_STR);
    return &fakeModule;
}
static void prepareDevice() {
    fakeDevice = {};
    fakeDevice.common.version = AUDIO_DEVICE_API_VERSION_3_2;
    fakeDevice.common.close = fakeDeviceClose;
    fakeDevice.set_mode = fakeSetMode;
    fakeDevice.set_parameters = fakeParameters;
    fakeDevice.get_parameters = fakeGetParameters;
    fakeDevice.open_output_stream = fakeOpenOutput;
    fakeDevice.close_output_stream = fakeCloseOutput;
    fakeDevice.open_input_stream = fakeInput;
    fakeDevice.close_input_stream = fakeCloseInput;
    fakeDevice.create_audio_patch = fakePatch;
    fakeDevice.release_audio_patch = fakeReleasePatch;
    fakeDevice.set_audio_port_config = fakePort;
    fakeModuleMethods.open = fakeOpenDevice;
    fakeModule = {};
    fakeModule.id = AUDIO_HARDWARE_MODULE_ID;
    fakeModule.methods = &fakeModuleMethods;
    unsupportedStream = false;
}

static audio_stream_out stream{};
static audio_stream_out second{};
static void reset() {
    acceptedConsent = true;
    std::lock_guard lock(mutex);
    if (lease >= 0) releaseLease("idle");
    outputs.clear(); unmanagedOutputs.clear(); patches.clear();
    device = nullptr; vendorLibrary = nullptr;
    hookReady = true; modeResult = 0;
    dspKnown = false; dspMutations = 0;
    dspRevoking = dspRevokePending = false; vendorWriteDepth = 0;
    phoneMode = AUDIO_MODE_NORMAL; requested = active = leaseMode = Mode::Off; preferenceLoaded = false;
    softwareGainLoaded = syncTrimLoaded = speakerOutputLoaded = false;
    softwareGainDb = appliedSoftwareGain = syncTrimMs = activeSyncTrimMs = 0;
    softwareGainUpdates = 0;
    speakerOutput = activeSpeakerOutput = lastSplitOutput = SpeakerOutput::Both;
    lease = -1; inputs = stockWrites = 0; reason = "idle"; playbackGate = "no_write";
    lastPreemption = "none"; lastPreemptionError = 0; preemptionCount = 0;
    leaseError = 0;
    capabilities = {};
    splitAvailable = false;
    splitSupported = true;
    currentRate = 48000;
    positionResult = 0; positionQueries = invalidPositionQuery = 0; invalidStamp = false;
    targetValid = true; splitSucceeds = true;
    receiverStarts = receiverPrepares = receiverCloses = receiverEnqueues = 0;
    receiverStartFails = receiverPrepareFails = receiverEnqueueFails = false;
    receiverPrepareError = EINVAL;
    traceReceiverLifecycle = false;
    receiverLifecycle.clear();
    receiverAsyncError = 0; receiverUnwrittenFrames = 0;
    receiverSamples.clear(); receiverFirstFrame = 0; receiverDeadline = 0;
    calculatedDelayNs = 0;
    vendorFrames.clear(); vendorBytes.clear(); vendorBuffer = nullptr;
    exactWriteResult = false;
    authorizedMode = "off";
    authorizedGain = "0";
    authorizedSoftwareGain = authorizedSyncTrim = "0"; authorizedOutput = "both";
    gainPreferenceLoaded = gainAvailable = false;
    receiverGainDb = reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    kernelGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    gainSupported = true; gainSetError = 0; revokeDuringGain = false; gainUpdates = 0;
    kernelState = 0; kernelStateReadError = 0; kernelGain = 65535; hookKnown = true; bypass = false;
    hookCalls = 0; mockGeneration = 0; hookGeneration = 0; mockScope = nullptr;
    inputCounts = {};
    currentFormat = AUDIO_FORMAT_PCM_16_BIT;
    fakeHandler = &handlerA; ambiguousWrite = false; observeHook = true;
    rejectAcquire = false; kernelExists = true;
    acquireError = EBUSY; leaseAttempts = 0;
    parameterResult = 0; revokeDuringParameter = false; parameterKernelState = 0;
    forwardedParameters.clear(); forwardedPort = nullptr;
    writeResult = 1; standbyResult = 0; standbyCalls = 0;
    writeReentry = standbyReentry = nullptr;
    startResult = resumeResult = stopResult = 0;
    withLifecycleCallbacks = false;
    activationReentry = nullptr;
    wideDiagnosticCounters = false;
    acquisitions = 0; lastBypass = false; events.clear();
    patchResult = releasePatchResult = 0; nextPatchHandle = 9;
    original = {};
    original.setMode = fakeSetMode;
    original.setParameters = fakeParameters;
    original.getParameters = fakeGetParameters;
    original.createPatch = fakePatch;
    original.releasePatch = fakeReleasePatch;
    original.openInput = fakeInput;
    original.closeInput = fakeCloseInput;
    original.setPort = fakePort;
    original.closeOutput = fakeCloseOutput;
    stream = {}; second = {};
    stream.common.get_format = second.common.get_format = format;
    stream.common.get_channels = second.common.get_channels = channels;
    stream.common.get_sample_rate = second.common.get_sample_rate = sampleRate;
    stream.get_presentation_position = second.get_presentation_position = presentationPosition;
    Output state{10, AUDIO_DEVICE_OUT_SPEAKER, AUDIO_OUTPUT_FLAG_DEEP_BUFFER,
        false, true, false, false, nullptr, false, fakeWrite, fakeStandby, fakeStreamParameters,
        nullptr, nullptr};
    outputs.emplace(&stream, state);
}
static void writeOnce(audio_stream_out* target = &stream) {
    short samples[32]{};
    assert(writeOutput(target, samples, sizeof(samples)) == sizeof(samples));
    assert(!bypass);
}
static void enable(const char* mode = "dual_mono") {
    authorizedMode = mode;
    const std::string parameter = std::string("tetris_audio_mode=") + mode;
    assert(setParameters(nullptr, parameter.c_str()) == 0);
    writeOnce();
    assert(kernelState == 0); // First unmodified write primes the actual backend.
    writeOnce();
    assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
}
static size_t longestStatus = 0;
static void statusContains(const char* expected) {
    char* status = getParameters(nullptr, "tetris_audio");
    assert(status && std::strlen(status) <= 4096);
    longestStatus = std::max(longestStatus, std::strlen(status));
    if (const char* path = std::getenv("TETRIS_AUDIO_STATUS_FIXTURES")) {
        FILE* file = std::fopen(path, "a");
        assert(file);
        std::fprintf(file, "%s\n", status);
        assert(std::fclose(file) == 0);
    }
    if (!status || std::string(status).find(expected) == std::string::npos) {
        std::fprintf(stderr, "Missing status %s in %s\n", expected, status ? status : "(null)");
    }
    assert(status && std::string(status).find(expected) != std::string::npos);
    free(status);
}
static void setLegacyPortField(std::array<audio_port_config, 2>& ports,
                              size_t index, size_t offset, uint32_t value) {
    // Encode the vendor's 216-byte array independently of C++ struct indexing.
    assert(index < 2 && offset + sizeof(value) <= 216);
    std::memcpy(reinterpret_cast<unsigned char*>(ports.data()) + index * 216 + offset,
                &value, sizeof(value));
}
static audio_port_config speakerGain() {
    audio_port_config config{};
    config.config_mask = AUDIO_PORT_CONFIG_GAIN;
    config.type = AUDIO_PORT_TYPE_DEVICE;
    config.role = AUDIO_PORT_ROLE_SINK;
    config.ext.device.type = AUDIO_DEVICE_OUT_SPEAKER;
    config.gain.mode = AUDIO_GAIN_MODE_JOINT;
    config.gain.values[0] = -600;
    return config;
}
static constexpr char volumeParameters[] = "volumeDevice=2;volumeIndex=7;volumeStreamType=3";
static int volumeCallback(unsigned kind, const char* parameters = volumeParameters) {
    if (kind == 0) return setParameters(nullptr, parameters);
    if (kind == 1) return setOutputParameters(&stream.common, parameters);
    auto config = speakerGain();
    const int result = setPort(nullptr, &config);
    assert(forwardedPort == &config);
    return result;
}
static void testVolumeCallbacks() {
    // A volume update must not strand a backend after its PCM writes move to
    // the mixer worker, or alter/reorder the vendor's validated parameters.
    for (unsigned kind : {0u, 1u, 2u}) {
        for (const char* mode : {"stereo", "dual_mono"}) {
            reset(); enable(mode); observeHook = false;
            const auto generation = hookGeneration;
            const unsigned state = kernelState;
            assert(volumeCallback(kind) == 0);
            assert(parameterKernelState == state && kernelState == state);
            assert(hookGeneration == generation && outputs.at(&stream).hookHandler == nullptr);
            if (kind != 2) assert(forwardedParameters == volumeParameters);
            writeOnce();
            assert(kernelState == state && acquisitions == 1);
        }
        // A failed volume call may have partially changed the backend.
        reset(); enable("stereo"); parameterResult = -EIO;
        assert(volumeCallback(kind) == -EIO);
        assert(!kernelState && !hookGeneration && !outputs.at(&stream).hookHandler);
        assert(outputs.at(&stream).preempted);
        writeOnce(); writeOnce(); assert(acquisitions == 1);
        // Kernel ownership loss during the callback still stops the receiver immediately.
        reset(); enable("stereo"); revokeDuringParameter = true;
        assert(volumeCallback(kind) == 0);
        assert(!kernelState && !hookGeneration && !outputs.at(&stream).hookHandler);
        assert(!outputs.at(&stream).preempted);
        writeOnce(); writeOnce(); assert(acquisitions == 1);
        // A volume-only callback cannot bypass the fresh-route retry delay,
        // including repeated callbacks after the first one closed the lease.
        reset(); enable("stereo"); kernelState = 0;
        assert(volumeCallback(kind) == 0 && !parameterKernelState);
        assert(!outputs.at(&stream).preempted && !outputs.at(&stream).hookHandler);
        assert(volumeCallback(kind) == 0);
        assert(!outputs.at(&stream).preempted);
        writeOnce(); writeOnce(); assert(acquisitions == 1);
        // A syntactically valid volume update cannot preserve an ineligible route.
        for (unsigned scenario = 0; scenario < 7; ++scenario) {
            reset(); enable("stereo");
            switch (scenario) {
                case 0: phoneMode = AUDIO_MODE_IN_CALL; break;
                case 1: outputs.at(&stream).route = AUDIO_DEVICE_OUT_WIRED_HEADPHONE; break;
                case 2: outputs.at(&stream).media = false; break;
                case 3: inputs = 1; break;
                case 4: outputs.emplace(&second, outputs.at(&stream)); break;
                case 5: unmanagedOutputs.insert(&second); break;
                case 6: outputs.at(&stream).preempted = true; break;
            }
            assert(volumeCallback(kind) == 0);
            assert(!parameterKernelState && !kernelState && !hookGeneration);
        }
    }
    for (unsigned kind : {0u, 1u}) {
        for (const char* parameters : {
                "volumeDevice=2;volumeIndex=7;volumeStreamType=3;routing=4",
                "volumeDevice=2;volumeIndex=7;volumeStreamType=0",
                "volumeDevice=4;volumeIndex=7;volumeStreamType=3",
                "volumeDevice=6;volumeIndex=7;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=7;volumeStreamType=3;volumeIndex=8",
                "volumeDevice=2;volumeIndex=;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=-1;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=+1;volumeStreamType=3",
                "volumeDevice=2;volumeIndex= 1;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=1x;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=2147483648;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=999999999999999999999;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=7;volumeStreamType=3;",
                "volumeDevice=2;;volumeIndex=7;volumeStreamType=3",
                "volumeDevice=2;volumeIndex=7", "", "volumeIndex"}) {
            reset(); enable("stereo");
            assert(volumeCallback(kind, parameters) == 0);
            assert(!kernelState && !hookGeneration && !outputs.at(&stream).hookHandler);
        }
    }
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        reset(); enable("stereo"); auto config = speakerGain();
        switch (scenario) {
            case 0: config.config_mask |= AUDIO_PORT_CONFIG_SAMPLE_RATE; break;
            case 1: config.type = AUDIO_PORT_TYPE_MIX; break;
            case 2: config.role = AUDIO_PORT_ROLE_SOURCE; break;
            case 3: config.ext.device.type = AUDIO_DEVICE_OUT_WIRED_HEADPHONE; break;
            case 4: config.ext.device.address[0] = 'x'; break;
            case 5: config.gain.mode = AUDIO_GAIN_MODE_CHANNELS; break;
            case 6: config.gain.mode = static_cast<audio_gain_mode_t>(
                    AUDIO_GAIN_MODE_JOINT | AUDIO_GAIN_MODE_RAMP); break;
        }
        assert(setPort(nullptr, &config) == 0);
        assert(!parameterKernelState && !kernelState && !hookGeneration);
    }
    // The same allowlisted string on a different, inactive stream is not a
    // volume update of the stream owning the lease.
    reset(); enable("stereo"); auto other = outputs.at(&stream); other.active = false;
    outputs.emplace(&second, other);
    assert(setOutputParameters(&second.common, volumeParameters) == 0);
    assert(!parameterKernelState && !kernelState && !hookGeneration);
}
static void testDolbyVolumeNotification() {
    // The first volume notification can precede lease acquisition, arrive
    // between the priming and enabling writes, or follow a completed standby.
    // Unsupported notification keys must not permanently suspend that stream.
    for (const char* mode : {"dual_mono", "stereo"}) {
        for (unsigned phase = 0; phase < 3; ++phase) {
            reset();
            authorizedMode = mode;
            const auto parameter = std::string("tetris_audio_mode=") + mode;
            assert(setParameters(nullptr, parameter.c_str()) == 0);
            if (phase == 1) writeOnce();
            if (phase == 2) {
                writeOnce(); writeOnce();
                assert(standbyOutput(&stream.common) == 0);
            }
            const unsigned previousAcquisitions = acquisitions;
            const auto preparedReceiver = receiver.state();
            const auto preparedHandler = outputs.at(&stream).hookHandler;
            const bool primed = outputs.at(&stream).primed;
            assert(!kernelState && lease == -1 && !outputs.at(&stream).preempted);
            parameterResult = -EINVAL;
            for (const char* notification : {"volume_change=7;flags=8",
                    "flags=8;volume_change=8"}) {
                assert(setParameters(nullptr, notification) == -EINVAL);
                assert(forwardedParameters == notification);
                assert(!outputs.at(&stream).preempted);
                assert(preemptionCount == 0);
                assert(outputs.at(&stream).primed == primed);
                assert(outputs.at(&stream).hookHandler == preparedHandler);
                assert(receiver.state().open == preparedReceiver.open);
                assert(receiver.state().queuedFrames == preparedReceiver.queuedFrames);
                assert(!dspMutations);
            }
            writeOnce(); writeOnce();
            assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
            assert(acquisitions == previousAcquisitions + 1);
            statusContains("tetris_audio_playback_gate=eligible");
            statusContains("tetris_audio_last_preempt=none");
        }
    }
    // The exemption is specific to this known unsupported device notification.
    // Other failures still suspend a stream even before it owns a lease.
    for (const char* mode : {"dual_mono", "stereo"}) {
        for (const char* parameters : {"volume_change=1;flags=8;routing=4",
                "volume_change=1;flags=8;flags=8", "screen_state=on", volumeParameters}) {
            reset(); authorizedMode = mode;
            const auto parameter = std::string("tetris_audio_mode=") + mode;
            assert(setParameters(nullptr, parameter.c_str()) == 0);
            parameterResult = -EINVAL;
            assert(setParameters(nullptr, parameters) == -EINVAL);
            assert(outputs.at(&stream).preempted && preemptionCount == 1);
            writeOnce(); writeOnce();
            assert(!kernelState && !acquisitions);
            statusContains("tetris_audio_playback_gate=preempted");
            statusContains("tetris_audio_last_preempt=device_parameters");
            statusContains("tetris_audio_last_preempt_error=-22");
        }
        for (int error : {-EIO, -ENODEV, -ENOSYS}) {
            reset(); authorizedMode = mode;
            const auto parameter = std::string("tetris_audio_mode=") + mode;
            assert(setParameters(nullptr, parameter.c_str()) == 0);
            parameterResult = error;
            assert(setParameters(nullptr, "volume_change=1;flags=8") == error);
            assert(outputs.at(&stream).preempted && preemptionCount == 1);
            writeOnce(); writeOnce();
            assert(!kernelState && !acquisitions);
            statusContains("tetris_audio_last_preempt=device_parameters");
            const auto expectedError = std::string("tetris_audio_last_preempt_error=") +
                    std::to_string(error);
            statusContains(expectedError.c_str());
        }
    }
    // Actual DSPVolumeSynchronizer messages seen in the AudioFlinger dump.
    // The MTK HAL returns EINVAL for this unsupported Xiaomi notification.
    for (const char* mode : {"stereo", "dual_mono"}) {
        reset(); enable(mode); observeHook = false; parameterResult = -EINVAL;
        const auto generation = hookGeneration;
        const unsigned state = kernelState;
        for (const char* parameters : {"volume_change=0;flags=8", "volume_change=7;flags=8",
                "volume_change=15;flags=8", "flags=8;volume_change=2147483647"}) {
            assert(setParameters(nullptr, parameters) == -EINVAL);
            assert(forwardedParameters == parameters && parameterKernelState == state);
            assert(kernelState == state && hookGeneration == generation);
            assert(outputs.at(&stream).hookHandler == nullptr && !outputs.at(&stream).preempted);
            writeOnce(); assert(kernelState == state && acquisitions == 1);
        }
    }
    // Success is harmless too; unrelated failures remain fatal to the lease.
    reset(); enable("stereo");
    assert(setParameters(nullptr, "volume_change=4;flags=8") == 0 && kernelState == 2);
    for (int failure : {-EIO, -ENODEV, -ENOSYS}) {
        reset(); enable("stereo"); parameterResult = failure;
        assert(setParameters(nullptr, "volume_change=4;flags=8") == failure);
        assert(!kernelState && !hookGeneration && outputs.at(&stream).preempted);
        writeOnce(); writeOnce(); assert(acquisitions == 1);
    }
    // No exemption for invalid/mixed notifications, other parameter errors,
    // or a stream-level request that the helper never emits.
    for (const char* parameters : {
            "volume_change=-1;flags=8", "volume_change=+1;flags=8",
            "volume_change=2147483648;flags=8", "volume_change=1x;flags=8",
            "volume_change=;flags=8", "volume_change=1;flags=0", "volume_change=1;flags=9",
            "volume_change=1;flags=8;flags=8", "volume_change=1;volume_change=2;flags=8",
            "volume_change=1;flags=8;routing=4", "volume_change=1;flags=8;screen_state=off",
            "volume_change=1;flags=8;volumeDevice=2", "volume_change=1;flags=8;",
            "volume_change=1;;flags=8", "volume_change=1", "flags=8", volumeParameters}) {
        reset(); enable("stereo"); parameterResult = -EINVAL;
        assert(setParameters(nullptr, parameters) == -EINVAL);
        assert(!kernelState && !hookGeneration && !outputs.at(&stream).hookHandler);
        assert(outputs.at(&stream).preempted);
    }
    reset(); enable("stereo"); parameterResult = -EINVAL;
    assert(setOutputParameters(&stream.common, "volume_change=1;flags=8") == -EINVAL);
    assert(!kernelState && !hookGeneration && outputs.at(&stream).preempted);
    for (const char* mode : {"dual_mono", "stereo"}) {
        for (bool revokeBefore : {false, true}) {
            reset(); enable(mode); parameterResult = -EINVAL;
            if (revokeBefore) kernelState = 0;
            else revokeDuringParameter = true;
            assert(setParameters(nullptr, "volume_change=1;flags=8") == -EINVAL);
            assert(!kernelState && !hookGeneration && !outputs.at(&stream).hookHandler);
            assert(!outputs.at(&stream).preempted);
            statusContains("tetris_audio_last_preempt=kernel_state_changed");
            const auto preemptions = preemptionCount;
            const auto preemptCause = std::string(lastPreemption);
            assert(setParameters(nullptr, "volume_change=2;flags=8") == -EINVAL);
            assert(preemptionCount == preemptions && lastPreemption == preemptCause);
            writeOnce(); writeOnce(); assert(acquisitions == 1);
            statusContains("tetris_audio_playback_gate=lease_busy");
        }
    }
    reset(); enable("stereo"); phoneMode = AUDIO_MODE_IN_CALL; parameterResult = -EINVAL;
    assert(setParameters(nullptr, "volume_change=1;flags=8") == -EINVAL);
    assert(!parameterKernelState && !kernelState && !hookGeneration);
}
static void testReceiverGain() {
    reset(); authorizedGain = "-6";
    assert(setParameters(nullptr, "tetris_audio_gain=-6") == 0);
    assert(lease == -1 && gainUpdates == 0 && receiverGainDb == -6);
    statusContains("tetris_audio_gain_available=1");
    statusContains("tetris_audio_gain_db=-6");
    enable("stereo");
    assert(kernelGainDb == -6 && gainUpdates == 1);
    const auto generation = hookGeneration;
    for (int gain : {TETRIS_AUDIO_RECEIVER_MIN_DB, 0, TETRIS_AUDIO_RECEIVER_MAX_DB}) {
        authorizedGain = std::to_string(gain);
        const std::string parameters = "tetris_audio_gain=" + authorizedGain;
        assert(setParameters(nullptr, parameters.c_str()) == 0);
        assert(kernelGainDb == gain && kernelState == 2 && hookGeneration == generation);
        statusContains(("tetris_audio_gain_db=" + authorizedGain).c_str());
    }
    // The kernel's reported setting wins over the HAL cache while a lease is live.
    kernelGainDb = 2;
    statusContains("tetris_audio_gain_db=2");
    // Persisted preferences are reloaded and applied before activating a new lease.
    reset(); authorizedGain = "3"; enable();
    assert(kernelGainDb == 3 && receiverGainDb == 3);
    standbyOutput(&stream.common); writeOnce(); writeOnce();
    assert(kernelGainDb == 3 && gainUpdates == 2);
    // Restore previous higher preferences at +6, but reject new requests above it.
    for (const char* previous : {"7", "8"}) {
        reset(); authorizedGain = previous; enable();
        assert(kernelGainDb == 6 && receiverGainDb == 6);
        statusContains("tetris_audio_gain_db=6");
        const auto parameters = std::string("tetris_audio_gain=") + previous;
        assert(setParameters(nullptr, parameters.c_str()) == -EINVAL);
        assert(kernelState == 1 && kernelGainDb == 6);
    }
    for (const char* invalid : {"-11", "9", "15", "", "+1", "1x", " 1", "2147483648"}) {
        reset(); authorizedGain = invalid; enable("stereo");
        assert(kernelGainDb == 0);
        const std::string parameters = std::string("tetris_audio_gain=") + invalid;
        assert(setParameters(nullptr, parameters.c_str()) == -EINVAL);
        assert(kernelState == 2 && kernelGainDb == 0);
    }
    reset(); enable("stereo");
    assert(setParameters(nullptr, "tetris_audio_gain=1") == -EPERM);
    assert(kernelState == 2 && receiverGainDb == 0);
    authorizedGain = "invalid";
    assert(setParameters(nullptr, "tetris_audio_gain=0") == -EPERM);
    authorizedGain = "1";
    for (const char* mixed : {"tetris_audio_gain=1;tetris_audio_mode=stereo",
            "tetris_audio_gain=1;routing=4", "tetris_audio_gain=1;tetris_audio_gain=1",
            "tetris_audio_gain=1;", ";tetris_audio_gain=1"}) {
        assert(setParameters(nullptr, mixed) == -EINVAL);
        assert(kernelState == 2 && receiverGainDb == 0);
    }
    // An ABI-v1 kernel without the optional ioctls retains ordinary playback.
    reset(); gainSupported = false; authorizedGain = "4"; enable("stereo");
    assert(kernelState == 2 && kernelGainDb == 0 && !gainAvailable);
    statusContains("tetris_audio_gain_available=0");
    statusContains("tetris_audio_gain_db=0");
    assert(setParameters(nullptr, "tetris_audio_gain=4") == -ENOTSUP);
    assert(kernelState == 2 && acquisitions == 1);
    // A supported gain failure must prevent activation or revoke the live lease.
    reset(); gainSetError = EIO; authorizedMode = "stereo"; authorizedGain = "2";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce(); writeOnce(); assert(!kernelState && acquisitions == 0);
    assert(outputs.at(&stream).preempted);
    reset(); enable("stereo"); authorizedGain = "2"; gainSetError = EIO;
    assert(setParameters(nullptr, "tetris_audio_gain=2") == -EIO);
    assert(!kernelState && !hookGeneration && outputs.at(&stream).preempted);
    assert(receiverGainDb == 0);
    // A kernel rejecting the saved gain must remain identifiable after stock
    // writes continue. It is not evidence of another output using the route.
    for (const char* mode : {"dual_mono", "stereo"}) {
        reset(); authorizedMode = mode; authorizedGain = "6"; gainSetError = EINVAL;
        const auto parameters = std::string("tetris_audio_mode=") + mode;
        assert(setParameters(nullptr, parameters.c_str()) == 0);
        writeOnce(); writeOnce();
        statusContains("tetris_audio_reason=gain_error");
        statusContains("tetris_audio_last_preempt_error=-22");
        writeOnce(); writeOnce();
        assert(!kernelState && acquisitions == 0 && outputs.at(&stream).preempted);
        statusContains("tetris_audio_reason=gain_error");

        // A newly selected supported gain and an explicit mode retry recover.
        authorizedGain = "0"; gainSetError = 0;
        assert(setParameters(nullptr, "tetris_audio_gain=0") == 0);
        assert(setParameters(nullptr, parameters.c_str()) == 0);
        writeOnce(); writeOnce();
        assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
        statusContains("tetris_audio_reason=playing");
    }
    reset(); enable("dual_mono"); authorizedGain = "6"; gainSetError = EINVAL;
    assert(setParameters(nullptr, "tetris_audio_gain=6") == -EINVAL);
    writeOnce(); writeOnce();
    assert(!kernelState && outputs.at(&stream).preempted);
    statusContains("tetris_audio_reason=gain_error");
    for (bool revokeBefore : {false, true}) {
        reset(); enable("stereo"); authorizedGain = "-3";
        if (revokeBefore) kernelState = 0;
        else revokeDuringGain = true;
        assert(setParameters(nullptr, "tetris_audio_gain=-3") == 0);
        assert(!kernelState && !hookGeneration && !outputs.at(&stream).preempted);
        assert(receiverGainDb == -3);
        writeOnce(); writeOnce(); assert(acquisitions == 1);
    }
    // A late property load must not freeze the startup default.
    reset(); authorizedGain = ""; restorePreference();
    assert(!gainPreferenceLoaded);
    authorizedGain = "-2"; enable(); assert(kernelGainDb == -2);
}
template <typename Sample> static void testInputSamples(audio_format_t pcmFormat) {
    reset(); currentFormat = pcmFormat; enable("stereo"); inputCounts = {};
    for (const std::array<Sample, 2> samples : {
            std::array<Sample, 2>{0, 0}, std::array<Sample, 2>{1200, 1200},
            std::array<Sample, 2>{1200, 0}, std::array<Sample, 2>{0, -1200}}) {
        // HAL buffers need not be aligned for the host's integer loads.
        std::array<unsigned char, sizeof(samples) + 1> unaligned{};
        std::memcpy(unaligned.data() + 1, samples.data(), sizeof(samples));
        const auto before = unaligned;
        assert(writeOutput(&stream, unaligned.data() + 1, sizeof(samples)) == sizeof(samples));
        assert(unaligned == before);
    }
    assert(inputCounts.buffers == 4 && inputCounts.frames == 4 && inputCounts.different == 2);
    assert(inputCounts.leftNonzero == 2 && inputCounts.rightNonzero == 2);
    statusContains("tetris_audio_diagnostic_version=12");
    statusContains("tetris_audio_input_different=2");
    statusContains("tetris_audio_input_left_nonzero=2");
    statusContains("tetris_audio_input_right_nonzero=2");
    inputCounts = {};
    std::array<Sample, 160> large{};
    for (size_t frame = 0; frame < 80; ++frame) large[frame * 2] = 100;
    assert(writeOutput(&stream, large.data(), sizeof(large)) == sizeof(large));
    assert(inputCounts.buffers == 1 && inputCounts.frames == 64 && inputCounts.different == 64);
    assert(inputCounts.leftNonzero == 64 && inputCounts.rightNonzero == 0);
    assert(writeOutput(&stream, large.data(), sizeof(large) - 1) == sizeof(large) - 1);
    assert(writeOutput(&stream, large.data(), 0) == 0);
    assert(inputCounts.buffers == 1 && inputCounts.frames == 64);
}
static void testInputScope() {
    testInputSamples<int16_t>(AUDIO_FORMAT_PCM_16_BIT);
    testInputSamples<int32_t>(AUDIO_FORMAT_PCM_32_BIT);
    reset(); writeOnce(); assert(inputCounts.frames == 0);
    reset(); enable("dual_mono"); assert(inputCounts.frames == 0);
    reset(); authorizedMode = "stereo";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce(); assert(inputCounts.frames == 0); // Unmodified priming write.
    writeOnce(); assert(inputCounts.frames == 16);
    inputCounts = {}; kernelState = 0;
    writeOnce(); assert(inputCounts.frames == 0); // Kernel revoked ownership.
    reset(); enable("stereo"); inputCounts = {};
    assert(setMode(nullptr, AUDIO_MODE_IN_CALL) == 0);
    writeOnce(); assert(inputCounts.frames == 0);
    reset(); enable("stereo"); inputCounts = {};
    metadataOutput(&stream, nullptr);
    writeOnce(); assert(inputCounts.frames == 0);
    reset(); enable("stereo"); inputCounts = {};
    outputs.at(&stream).route = AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    writeOnce(); assert(inputCounts.frames == 0);
    reset(); enable("stereo"); inputCounts = {};
    currentFormat = AUDIO_FORMAT_PCM_FLOAT;
    assert(writeOutput(&stream, reinterpret_cast<void*>(1), 4) == 4);
    assert(inputCounts.frames == 0); // Unsupported format is never inspected.
    reset(); rejectAcquire = true; authorizedMode = "stereo";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce(); writeOnce(); assert(inputCounts.frames == 0);
}
template <typename Sample> static void testSplitAcceptedFrames(audio_format_t pcmFormat) {
    reset(); currentFormat = pcmFormat; enable("stereo");
    const std::array<Sample, 8> samples{123, -456, -789, 321, 12, -34, -56, 78};
    const auto originalSamples = samples;
    const uint64_t firstFrame = outputs.at(&stream).writtenFrames;
    const unsigned queued = receiverEnqueues;
    exactWriteResult = true;
    writeResult = 2 * 2 * sizeof(Sample); // Vendor accepts only two of four frames.
    assert(writeOutput(&stream, samples.data(), sizeof(samples)) == writeResult);
    assert(samples == originalSamples && vendorBuffer != samples.data());
    std::array<Sample, 8> lower{};
    std::memcpy(lower.data(), vendorBytes.data(), sizeof(lower));
    assert((lower == std::array<Sample, 8>{0, -456, 0, 321, 0, -34, 0, 78}));
    const int64_t scale = sizeof(Sample) == 2 ? 65536 : 1;
    assert((receiverSamples == std::vector<int32_t>{int32_t(123 * scale), int32_t(-789 * scale)}));
    assert(receiverEnqueues == queued + 1 && receiverFirstFrame == firstFrame);
    assert(outputs.at(&stream).writtenFrames == firstFrame + 2);
    assert(receiverDeadline == calculatedDeadline && receiverDeadline > 0);
    assert(!lastBypass && dspMutations == 0);
    // Returning an already-consumed prefix is essential: never retry it after
    // a receiver failure or duplicate rejected tail frames on the second sink.
    receiverEnqueueFails = true;
    assert(writeOutput(&stream, samples.data(), sizeof(samples)) == writeResult);
    assert(outputs.at(&stream).writtenFrames == firstFrame + 4);
    assert(!kernelState && !receiver.state().open && outputs.at(&stream).preempted);
    assert(receiverCloses == 1);
    statusContains("tetris_audio_reason=sync_error");
}
static void testStereoSplit() {
    testSplitAcceptedFrames<int16_t>(AUDIO_FORMAT_PCM_16_BIT);
    testSplitAcceptedFrames<int32_t>(AUDIO_FORMAT_PCM_32_BIT);
    for (ssize_t accepted : {ssize_t(0), ssize_t(-EAGAIN), ssize_t(3), ssize_t(68)}) {
        reset(); enable("stereo");
        const auto before = outputs.at(&stream).writtenFrames;
        const unsigned enqueues = receiverEnqueues;
        short samples[32]{};
        exactWriteResult = true; writeResult = accepted;
        assert(writeOutput(&stream, samples, sizeof(samples)) == accepted);
        assert(outputs.at(&stream).writtenFrames == before);
        assert(receiverEnqueues == enqueues && !kernelState && !receiver.state().open);
        assert(outputs.at(&stream).frameAccountingValid == (accepted <= 0));
    }
    // DSP-specific OPEN/INIT flags and downmix bypass are not required to
    // split a stream whose route and media metadata are already admitted.
    reset(); dspKnown = false; observeHook = false; enable("stereo");
    assert(receiverStarts == 1 && !standbyCalls && !dspMutations);
    assert(!lastBypass && receiver.state().running);
    for (bool missingCaps : {false, true}) {
        reset(); splitSupported = !missingCaps;
        authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") ==
                (missingCaps ? -ENOTSUP : 0));
        if (!missingCaps) {
            receiverStartFails = true; writeOnce(); writeOnce();
            assert(!kernelState && !receiver.state().open && outputs.at(&stream).preempted);
        }
    }
    reset(); splitSupported = false; enable("dual_mono");
    assert(!receiverStarts && !receiver.state().open);
}
static void testStereoStartupOrdering() {
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset(); authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        writeOnce(); // Prime the main output, still unmodified.
        traceReceiverLifecycle = true;
        receiverStartFails = failure == 1;
        rejectAcquire = failure == 2;
        receiverPrepareFails = failure == 3;
        const short samples[]{123, -456, -789, 321};
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        if (failure == 0) {
            assert((receiverLifecycle == std::vector<std::string>{
                "start", "route_on", "prepare", "vendor_write"}));
            assert(kernelState == 2 && receiverPrepares == 1 && receiverEnqueues == 1);
            assert(vendorBuffer != samples && outputs.at(&stream).writtenFrames == 18);
            continue;
        }
        assert(vendorBuffer == samples && receiverEnqueues == 0);
        assert(!kernelState && lease == -1 && !receiver.state().open);
        assert(active == Mode::Off && outputs.at(&stream).preempted == (failure != 2));
        assert(outputs.at(&stream).writtenFrames == 18);
        if (failure == 1) {
            assert((receiverLifecycle == std::vector<std::string>{
                "start", "lease_close", "vendor_write"}));
            assert(!receiverPrepares && !acquisitions && !receiverCloses);
        } else if (failure == 2) {
            assert((receiverLifecycle == std::vector<std::string>{
                "start", "route_failed", "receiver_close", "lease_close", "vendor_write"}));
            assert(!receiverPrepares && !acquisitions && receiverCloses == 1);
        } else {
            assert((receiverLifecycle == std::vector<std::string>{
                "start", "route_on", "prepare", "receiver_close", "lease_close", "vendor_write"}));
            assert(receiverPrepares == 1 && acquisitions == 1 && receiverCloses == 1);
            assert(leaseError == -EINVAL && receiver.state().error == EINVAL);
            statusContains("tetris_audio_reason=receiver_unavailable");
            statusContains("tetris_audio_last_preempt_error=-22");
        }
    }
    reset(); enable("dual_mono");
    assert(!receiverStarts && !receiverPrepares);
}
static void testStartupClockRevalidation() {
    reset(); authorizedMode = "stereo";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce();
    invalidPositionQuery = 2; // Initial clock valid, post-prepare clock invalid.
    traceReceiverLifecycle = true;
    const short samples[]{123, -456, -789, 321};
    assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
    assert(positionQueries == 2 && receiverPrepares == 1);
    assert((receiverLifecycle == std::vector<std::string>{
        "start", "route_on", "prepare", "receiver_close", "lease_close", "vendor_write"}));
    assert(vendorBuffer == samples && receiverEnqueues == 0);
    assert(!kernelState && lease == -1 && !receiver.state().open);
    assert(active == Mode::Off && !outputs.at(&stream).preempted && preemptionCount == 0);
    assert(outputs.at(&stream).writtenFrames == 18);
    statusContains("tetris_audio_reason=sync_waiting");
    // A later valid clock can resume directly from the frame following the
    // stock fallback block; no old left-channel samples are replayed.
    writeOnce();
    assert(kernelState == 2 && receiverStarts == 2 && receiverPrepares == 2);
    assert(receiverEnqueues == 1 && receiverFirstFrame == 18);
    assert(!outputs.at(&stream).preempted && preemptionCount == 0);
}
static void testReceiverSegmentDrain() {
    // After the receiver accepted every frame and its playout deadline passed,
    // a FIFO underrun is a natural pause. Fresh writes get a new clock without
    // replaying the old left channel or permanently suspending the selection.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        reset(); enable("stereo");
        const unsigned enqueues = receiverEnqueues;
        const auto mainFrames = outputs.at(&stream).writtenFrames;
        assert(acceptedReceiverFrames > 0);
        assert(receiver.state().writtenFrames == acceptedReceiverFrames);
        receiverAsyncError = scenario == 2 ? EIO : EPIPE;
        receiverUnwrittenFrames = scenario == 1 ? 1 : 0;
        timespec now{};
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        const int64_t nowNs = int64_t(now.tv_sec) * 1000000000LL + now.tv_nsec;
        if (receiverSegmentEndNs >= nowNs) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(
                    receiverSegmentEndNs - nowNs + 1000000));
        }
        assert(checkLease());
        assert(!kernelState && !receiver.state().open && receiverCloses == 1);
        assert(receiverEnqueues == enqueues && outputs.at(&stream).writtenFrames == mainFrames);
        assert(!outputs.at(&stream).primed);
        if (scenario == 0) {
            assert(!outputs.at(&stream).preempted && preemptionCount == 0);
            writeOnce();
            assert(!kernelState && receiverEnqueues == enqueues && receiverStarts == 1);
            writeOnce();
            assert(kernelState == 2 && receiverStarts == 2 && receiverEnqueues == enqueues + 1);
            assert(receiverFirstFrame == mainFrames + 16);
            assert(!outputs.at(&stream).preempted && preemptionCount == 0);
        } else {
            assert(outputs.at(&stream).preempted && preemptionCount == 1);
            statusContains("tetris_audio_last_preempt=receiver_clock");
            statusContains("tetris_audio_reason=sync_error");
            writeOnce(); writeOnce();
            assert(!kernelState && receiverStarts == 1 && receiverEnqueues == enqueues);
        }
    }
}
static void testReceiverCalibrationScope() {
    for (const auto flags : {AUDIO_OUTPUT_FLAG_DEEP_BUFFER, AUDIO_OUTPUT_FLAG_NONE,
                            AUDIO_OUTPUT_FLAG_PRIMARY, AUDIO_OUTPUT_FLAG_FAST,
                            static_cast<audio_output_flags_t>(
                                    AUDIO_OUTPUT_FLAG_DEEP_BUFFER | AUDIO_OUTPUT_FLAG_PRIMARY)}) {
        for (const auto format : {AUDIO_FORMAT_PCM_16_BIT, AUDIO_FORMAT_PCM_32_BIT}) {
            reset();
            currentFormat = format;
            outputs.at(&stream).flags = flags;
            enable("stereo");
            const int64_t expected = flags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER &&
                    format == AUDIO_FORMAT_PCM_32_BIT ? 72000000 : 0;
            assert(calculatedDelayNs == expected && receiverDelayNs == expected);
            assert(receiverDeadline == calculatedDeadline);
            // Calibration belongs to the separate receiver queue. The original
            // vendor position callback and accepted-frame accounting stay intact.
            uint64_t frames = 0;
            timespec stamp{};
            assert(stream.get_presentation_position == presentationPosition);
            assert(stream.get_presentation_position(&stream, &frames, &stamp) == 0);
            assert(frames == vendorFrames[&stream]);
            assert(outputs.at(&stream).writtenFrames == frames);
            const auto diagnostic = "tetris_audio_split_delay_ns=" + std::to_string(expected);
            statusContains(diagnostic.c_str());
            assert(standbyOutput(&stream.common) == 0);
            statusContains("tetris_audio_split_delay_ns=0");
        }
    }
    reset(); currentFormat = AUDIO_FORMAT_PCM_32_BIT;
    enable("dual_mono");
    assert(positionQueries == 0 && receiverEnqueues == 0 && receiverDelayNs == 0);
}
static void testPresentationClock() {
    for (unsigned failure = 0; failure < 6; ++failure) {
        reset(); authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        switch (failure) {
            case 0: positionResult = -ENOSYS; break;
            case 1: invalidStamp = true; break;
            case 2: targetValid = false; break;
            case 3: currentRate = 44100; break;
            case 4: stream.get_presentation_position = nullptr; break;
            case 5: stream.common.get_sample_rate = nullptr; break;
        }
        const short samples[]{11, 22, 33, 44};
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        assert(vendorBuffer == samples && !receiverStarts && !kernelState);
        assert(!outputs.at(&stream).preempted);
    }
    // A missing clock during live stereo revokes the route; the offered block
    // still reaches stock playback unchanged and must not be replayed.
    reset(); enable("stereo"); positionResult = -EIO;
    const short samples[]{11, 22, 33, 44};
    const unsigned enqueues = receiverEnqueues;
    assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
    assert(vendorBuffer == samples && !kernelState && !receiver.state().open);
    assert(receiverEnqueues == enqueues && outputs.at(&stream).preempted);
    statusContains("tetris_audio_last_preempt=presentation_clock");
    // The main stream's cumulative count includes OFF, priming, partial and
    // fallback writes and survives standby. It is not a receiver-only cursor.
    reset(); writeOnce(); writeOnce();
    assert(outputs.at(&stream).writtenFrames == 32);
    enable("stereo"); assert(receiverFirstFrame == 48);
    assert(standbyOutput(&stream.common) == 0);
    assert(outputs.at(&stream).writtenFrames == 64);
    writeOnce(); writeOnce(); assert(receiverFirstFrame == 80);
    reset(); exactWriteResult = true; writeResult = 3;
    assert(writeOutput(&stream, samples, sizeof(samples)) == 3);
    assert(!outputs.at(&stream).frameAccountingValid);
    exactWriteResult = false; writeResult = 1; authorizedMode = "stereo";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce(); writeOnce(); assert(!kernelState && !receiverStarts);
}
static void testReceiverRevocation() {
    for (unsigned event = 0; event < 7; ++event) {
        reset(); enable("stereo");
        original.closeOutput = fakeCloseOutput;
        outputs.at(&stream).metadata = fakeMetadata;
        outputs.at(&stream).metadataV7 = fakeMetadataV7;
        events.clear();
        switch (event) {
            case 0: assert(setMode(nullptr, AUDIO_MODE_IN_CALL) == 0); break;
            case 1: assert(setOutputParameters(&stream.common, "routing=4") == 0); break;
            case 2: assert(standbyOutput(&stream.common) == 0); break;
            case 3: closeOutput(nullptr, &stream); break;
            case 4: metadataOutput(&stream, nullptr); break;
            case 5: metadataOutputV7(&stream, nullptr); break;
            case 6:
                authorizedMode = "off";
                assert(setParameters(nullptr, "tetris_audio_mode=off") == 0);
                break;
        }
        assert(!kernelState && !receiver.state().open && receiverCloses == 1);
    }
    // The original vendor write may synchronously change route or metadata.
    // Account the accepted bytes, suppress the receiver tail, and defer close
    // until control leaves that callback; never manufacture an EIO replay.
    for (bool metadata : {false, true}) {
        reset(); enable("stereo");
        const auto before = outputs.at(&stream).writtenFrames;
        const unsigned enqueues = receiverEnqueues;
        outputs.at(&stream).metadata = fakeMetadata;
        if (metadata) writeReentry = [] { metadataOutput(&stream, nullptr); };
        else writeReentry = [] {
            assert(setParameters(nullptr, "screen_state=on") == -EIO);
        };
        short samples[32]{};
        events.clear();
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        assert(outputs.at(&stream).writtenFrames == before + 16);
        assert(outputs.at(&stream).preempted && !kernelState && !receiver.state().open);
        assert(receiverEnqueues == enqueues && receiverCloses == 1);
        assert(std::find(events.begin(), events.end(), "metadata") == events.end());
    }
}
static void testStereoAdmissionGuards() {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        reset(); requested = Mode::Stereo; preferenceLoaded = true;
        auto& output = outputs.at(&stream);
        switch (scenario) {
            case 0: phoneMode = AUDIO_MODE_IN_CALL; break;
            case 1: output.route = AUDIO_DEVICE_OUT_WIRED_HEADPHONE; break;
            case 2: output.media = false; break;
            case 3: inputs = 1; break;
            case 4: { auto other = output; other.active = true;
                      outputs.emplace(&second, other); break; }
            case 5: unmanagedOutputs.insert(&second); break;
            case 6: output.flags = AUDIO_OUTPUT_FLAG_DIRECT; break;
            case 7: currentFormat = AUDIO_FORMAT_PCM_FLOAT; break;
            case 8: output.preempted = true; break;
            case 9: currentRate = 44100; break;
            case 10: stockWrites = 1; break;
        }
        writeOnce(); writeOnce();
        assert(!receiverStarts && !acquisitions && !standbyCalls && !kernelState && !lastBypass);
        stockWrites = 0;
    }
}
static void testIdleOutputDoesNotBlockPlayback() {
    // AudioFlinger can park an offload output for later reuse while ordinary
    // PCM media is playing. Merely opening it is not concurrent playback.
    for (const char* mode : {"dual_mono", "stereo"}) {
        reset(); authorizedMode = mode;
        const auto parameter = std::string("tetris_audio_mode=") + mode;
        assert(setParameters(nullptr, parameter.c_str()) == 0);
        auto parked = outputs.at(&stream);
        parked.active = false;
        parked.primed = false;
        parked.flags = AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
        outputs.emplace(&second, parked);
        writeOnce(); writeOnce();
        assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
        assert(acquisitions == 1);
        statusContains("tetris_audio_playback_gate=eligible");
    }

    // A kernel revocation belongs to the playing owner. A dormant PCM output
    // must not inherit an unrelated preemption when it later starts playback.
    reset(); enable("dual_mono");
    auto dormant = outputs.at(&stream);
    dormant.active = false; dormant.primed = false;
    outputs.emplace(&second, dormant);
    kernelState = 0;
    writeOnce();
    assert(!outputs.at(&stream).preempted && !outputs.at(&second).preempted);
    assert(outputs.at(&stream).leaseRetryAfter != std::chrono::steady_clock::time_point{});
    assert(outputs.at(&second).leaseRetryAfter == std::chrono::steady_clock::time_point{});
    assert(standbyOutput(&stream.common) == 0);
    writeOnce(&second); writeOnce(&second);
    assert(kernelState == 1 && acquisitions == 2);

    // An ordinary transient vendor write error is not a permanent route loss.
    // Dual-mono should recover by priming again, without changing the setting.
    reset(); enable("dual_mono"); writeResult = -EAGAIN;
    short samples[32]{};
    assert(writeOutput(&stream, samples, sizeof(samples)) == -EAGAIN);
    assert(!kernelState && !outputs.at(&stream).preempted);
    writeResult = 1;
    writeOnce(); assert(!kernelState);
    writeOnce(); assert(kernelState == 1 && acquisitions == 2);
}
static void testAsynchronousPlaybackCallbacks() {
    // The offload descriptor is initially parked. Actual resume/start must
    // close the receiver before the vendor starts asynchronous audio.
    for (bool useStart : {false, true}) {
        reset(); original.openOutput = fakeOpenOutput;
        original.closeOutput = fakeCloseOutput;
        withLifecycleCallbacks = true;
        audio_stream_out* parked = nullptr;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, nullptr, &parked, "") == 0);
        assert(parked->start == startOutput && parked->resume == resumeOutput &&
               parked->stop == stopOutput);
        enable("stereo");
        events.clear();
        assert((useStart ? parked->start(parked) : parked->resume(parked)) == 0);
        assert((events == std::vector<std::string>{"restore", useStart ? "start" : "resume"}));
        assert(outputs.at(parked).active && !dspMutations && !kernelState);
        writeOnce(); writeOnce();
        assert(!kernelState && receiverStarts == 1);
        statusContains("tetris_audio_playback_gate=other_output");
        // Settings or metadata updates cannot pretend that queued offload
        // playback has stopped merely because proxy bookkeeping was reset.
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        assert(outputs.at(parked).active);
        writeOnce(); assert(!kernelState && receiverStarts == 1);
        events.clear();
        assert(parked->stop(parked) == 0);
        assert((events == std::vector<std::string>{"stop"}));
        assert(!outputs.at(parked).active);
        writeOnce(); writeOnce();
        assert(kernelState == 2 && receiverStarts == 2);
    }

    reset(); original.openOutput = fakeOpenOutput;
    audio_stream_out* plain = nullptr;
    assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                      AUDIO_OUTPUT_FLAG_DEEP_BUFFER, nullptr, &plain, "") == 0);
    // A missing optional vendor callback must stay absent in the exposed ABI.
    assert(!plain->start && !plain->resume && !plain->stop);
}
static void testQueuedPlaybackActivity() {
    // Once asynchronous playback starts, a short/failed buffer submission
    // cannot tell us whether the DSP has finished the previously queued audio.
    for (const char* mode : {"off", "stereo"}) {
        for (ssize_t result : {ssize_t(0), ssize_t(-EAGAIN)}) {
            reset(); original.openOutput = fakeOpenOutput;
            withLifecycleCallbacks = true;
            audio_stream_out* queued = nullptr;
            assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                              AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, nullptr, &queued, "") == 0);
            authorizedMode = mode;
            const auto parameter = std::string("tetris_audio_mode=") + mode;
            assert(setParameters(nullptr, parameter.c_str()) == 0);
            assert(queued->resume(queued) == 0);
            writeResult = result;
            short samples[32]{};
            assert(queued->write(queued, samples, sizeof(samples)) == result);
            assert(outputs.at(queued).active);
            writeResult = 1;
            authorizedMode = "stereo";
            assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
            writeOnce(); writeOnce();
            assert(!kernelState && !receiverStarts);
            standbyResult = -EIO;
            assert(queued->common.standby(&queued->common) == -EIO);
            assert(outputs.at(queued).active);
            writeOnce(); assert(!receiverStarts);
            standbyResult = 0;
            stopResult = -EIO;
            assert(queued->stop(queued) == -EIO && outputs.at(queued).active);
            writeOnce(); assert(!receiverStarts);
            stopResult = 0;
            assert(queued->stop(queued) == 0 && !outputs.at(queued).active);
            writeOnce(); writeOnce();
            assert(kernelState == 2 && receiverStarts == 1);
        }
    }
    // During the original start/resume callback the asynchronous stream may
    // already be feeding hardware. A nested PCM write cannot acquire the receiver.
    for (bool useStart : {false, true}) {
        reset(); original.openOutput = fakeOpenOutput;
        withLifecycleCallbacks = true;
        audio_stream_out* queued = nullptr;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, nullptr, &queued, "") == 0);
        authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        activationReentry = [] {
            short samples[32]{};
            const auto result = writeOutput(&stream, samples, sizeof(samples));
            assert(result == -EIO || result == sizeof(samples));
            assert(!receiverStarts && !kernelState);
        };
        assert((useStart ? queued->start(queued) : queued->resume(queued)) == 0);
        assert(outputs.at(queued).active && !receiverStarts);
    }
    // A failed activation can still have started a vendor worker before the
    // error return. Wait for an explicit successful stop/standby to clear it.
    for (bool useStart : {false, true}) {
        reset(); original.openOutput = fakeOpenOutput;
        withLifecycleCallbacks = true;
        audio_stream_out* queued = nullptr;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, nullptr, &queued, "") == 0);
        authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        startResult = resumeResult = -EAGAIN;
        assert((useStart ? queued->start(queued) : queued->resume(queued)) == -EAGAIN);
        assert(outputs.at(queued).active);
        writeOnce(); writeOnce();
        assert(!receiverStarts && !kernelState);
        assert(queued->stop(queued) == 0);
        writeOnce(); writeOnce();
        assert(receiverStarts == 1 && kernelState == 2);
    }
    // A positive first write can queue offload audio before a reentrant event.
    // Report accepted bytes and keep it active until a successful stop.
    for (bool metadataEvent : {false, true}) {
        reset(); original.openOutput = fakeOpenOutput;
        withLifecycleCallbacks = true;
        audio_stream_out* queued = nullptr;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, nullptr, &queued, "") == 0);
        authorizedMode = "stereo";
        assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
        if (metadataEvent) {
            writeReentry = [] { metadataOutput(&abiStream, nullptr); };
        } else {
            writeReentry = [] {
                assert(setParameters(nullptr, "screen_state=on") == -EIO);
            };
        }
        short samples[32]{};
        assert(queued->write(queued, samples, sizeof(samples)) == sizeof(samples));
        assert(outputs.at(queued).active && outputs.at(queued).preempted);
        assert(!outputs.at(&stream).preempted);
        writeOnce(); writeOnce();
        assert(!receiverStarts && !kernelState);
        statusContains("tetris_audio_playback_gate=other_output");
        assert(queued->stop(queued) == 0);
        writeOnce(); writeOnce();
        assert(receiverStarts == 1 && kernelState == 2);
    }
}
static void testUnsupportedMmapStart() {
    // The audited vendor start() returns ENOSYS before touching hardware when
    // that operation is unsupported. An idle descriptor must stay idle then.
    for (const char* mode : {"dual_mono", "stereo"}) {
        reset(); original.openOutput = fakeOpenOutput;
        withLifecycleCallbacks = true;
        audio_stream_out* mmap = nullptr;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_MMAP_NOIRQ, nullptr, &mmap, "") == 0);
        authorizedMode = mode;
        const auto parameter = std::string("tetris_audio_mode=") + mode;
        assert(setParameters(nullptr, parameter.c_str()) == 0);
        startResult = -ENOSYS;
        events.clear();
        assert(mmap->start(mmap) == -ENOSYS);
        assert((events == std::vector<std::string>{"start"}));
        assert(!outputs.at(mmap).active);
        writeOnce(); writeOnce();
        assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
        assert(acquisitions == 1);
        statusContains("tetris_audio_playback_gate=eligible");

        // Unsupported start is not evidence that previously started playback
        // has ended. Preserve that state until successful stop or standby.
        reset(); original.openOutput = fakeOpenOutput;
        withLifecycleCallbacks = true;
        assert(openOutput(nullptr, 20, AUDIO_DEVICE_OUT_SPEAKER,
                          AUDIO_OUTPUT_FLAG_MMAP_NOIRQ, nullptr, &mmap, "") == 0);
        authorizedMode = mode;
        assert(setParameters(nullptr, parameter.c_str()) == 0);
        assert(mmap->start(mmap) == 0 && outputs.at(mmap).active);
        startResult = -ENOSYS;
        assert(mmap->start(mmap) == -ENOSYS && outputs.at(mmap).active);
        writeOnce(); writeOnce();
        assert(!kernelState && !acquisitions && !receiverStarts);
        statusContains("tetris_audio_playback_gate=other_output");
        assert(mmap->stop(mmap) == 0 && !outputs.at(mmap).active);
        writeOnce(); writeOnce();
        assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
    }
}
static void expireLeaseRetry() {
    outputs.at(&stream).leaseRetryAfter = std::chrono::steady_clock::now() -
            std::chrono::milliseconds(1);
}
static void requestPlayback(const char* mode) {
    authorizedMode = mode;
    const auto parameter = std::string("tetris_audio_mode=") + mode;
    assert(setParameters(nullptr, parameter.c_str()) == 0);
    writeOnce(); writeOnce();
}
static void testTransientLeaseRecovery() {
    for (const char* mode : {"dual_mono", "stereo"}) for (int error : {EBUSY, EAGAIN}) {
        reset(); rejectAcquire = true; acquireError = error;
        requestPlayback(mode);
        assert(leaseAttempts == 1 && !kernelState && lease < 0);
        assert(!receiver.state().open && !outputs.at(&stream).preempted);
        assert(receiverEnqueues == 0);
        statusContains("tetris_audio_playback_gate=lease_busy");
        // Every retry must leave the ordinary vendor playback intact, and
        // must not open/prepare the receiver again on every PCM buffer.
        const auto attempts = leaseAttempts, starts = receiverStarts;
        const auto firstFrame = outputs.at(&stream).writtenFrames;
        for (int i = 0; i < 10; ++i) writeOnce();
        assert(leaseAttempts == attempts && receiverStarts == starts);
        assert(outputs.at(&stream).writtenFrames > firstFrame);
        expireLeaseRetry(); writeOnce();
        assert(leaseAttempts == attempts + 1 && !kernelState && lease < 0);
        rejectAcquire = false;
        writeOnce(); assert(!kernelState); // Cooldown still applies after contention ends.
        expireLeaseRetry(); writeOnce();
        assert(kernelState == (std::string(mode) == "stereo" ? 2u : 1u));
        assert(!outputs.at(&stream).preempted);
        assert(outputs.at(&stream).leaseRetryAfter == std::chrono::steady_clock::time_point{});
        statusContains("tetris_audio_playback_gate=eligible");
        statusContains("tetris_audio_reason=playing");
    }

    // Expiring a timer grants no permission: all current guards still apply.
    for (unsigned gate = 0; gate < 11; ++gate) {
        reset(); rejectAcquire = true; requestPlayback("stereo");
        rejectAcquire = false; expireLeaseRetry();
        switch (gate) {
            case 0: phoneMode = AUDIO_MODE_IN_CALL; break;
            case 1: outputs.at(&stream).route = AUDIO_DEVICE_OUT_BLUETOOTH_A2DP; break;
            case 2: outputs.at(&stream).media = false; break;
            case 3: {
                auto other = outputs.at(&stream); other.active = true;
                outputs.emplace(&second, other); break;
            }
            case 4: inputs = 1; break;
            case 5: currentFormat = AUDIO_FORMAT_PCM_FLOAT; break;
            case 6: preemptOutput(outputs.at(&stream), "kernel_state_changed"); break;
            case 7: authorizedMode = "off";
                    assert(setParameters(nullptr, "tetris_audio_mode=off") == 0); break;
            case 8: unmanagedOutputs.insert(&second); break;
            case 9: currentRate = 44100; break;
            case 10: targetValid = false; break;
        }
        const auto attempts = leaseAttempts, starts = receiverStarts;
        writeOnce(); writeOnce();
        assert(leaseAttempts == attempts && receiverStarts == starts && !kernelState);
        assert(receiverEnqueues == 0);
    }
    for (int error : {EINVAL, EIO, EPIPE, ENODEV}) {
        reset(); rejectAcquire = true; acquireError = error; requestPlayback("stereo");
        assert(outputs.at(&stream).preempted);
        rejectAcquire = false; expireLeaseRetry();
        writeOnce(); writeOnce();
        assert(leaseAttempts == 1 && !kernelState && receiverEnqueues == 0);
    }
    // Preparation can be temporarily busy after the kernel connects DL8.
    // Close the failed route completely before acquiring a new clock/session.
    reset(); receiverPrepareFails = true; receiverPrepareError = EAGAIN;
    requestPlayback("stereo");
    assert(!kernelState && lease < 0 && !receiver.state().open && !receiverEnqueues);
    assert(!outputs.at(&stream).preempted && receiverCloses == 1);
    receiverPrepareFails = false; expireLeaseRetry(); writeOnce();
    assert(kernelState == 2 && receiverStarts == 2 && receiverEnqueues == 1);
}
static void testStockRouteRevocationRecovery() {
    // A short-lived stock route update must not require music to enter standby.
    // The capture/output/metadata boundary may arrive after the kernel revokes
    // the receiver, including an EPIPE caused by disconnecting its PCM route.
    for (const char* mode : {"dual_mono", "stereo"}) {
        for (unsigned transition = 0; transition < 4; ++transition) {
            reset(); enable(mode);
            const bool stereo = std::string(mode) == "stereo";
            const unsigned before = acquisitions;
            kernelState = 0;
            if (stereo) receiverAsyncError = EPIPE;
            writeOnce();
            assert(!kernelState && lease < 0 && !receiver.state().open);
            assert(!outputs.at(&stream).preempted);
            assert(outputs.at(&stream).leaseRetryAfter != std::chrono::steady_clock::time_point{});
            const auto acceptedBeforeRecovery = outputs.at(&stream).writtenFrames;
            for (unsigned i = 0; i < 10; ++i) writeOnce();
            assert(acquisitions == before && !kernelState);

            playback_track_metadata notificationTrack{
                AUDIO_USAGE_NOTIFICATION, AUDIO_CONTENT_TYPE_SONIFICATION, 1.f};
            source_metadata notificationMetadata{1, &notificationTrack};
            playback_track_metadata musicTrack{AUDIO_USAGE_MEDIA, AUDIO_CONTENT_TYPE_MUSIC, 1.f};
            source_metadata musicMetadata{1, &musicTrack};
            if (transition == 1) {
                assert(openInput(nullptr, 1, AUDIO_DEVICE_IN_BUILTIN_MIC, nullptr, nullptr,
                                 AUDIO_INPUT_FLAG_NONE, "", AUDIO_SOURCE_CAMCORDER) == 0);
            } else if (transition == 2) {
                auto notification = outputs.at(&stream);
                notification.handle = 20;
                notification.active = true;
                notification.media = false;
                notification.leaseRetryAfter = {};
                outputs.emplace(&second, notification);
            } else if (transition == 3) {
                metadataOutput(&stream, &notificationMetadata);
            }
            if (transition) {
                expireLeaseRetry();
                writeOnce(); writeOnce();
                assert(acquisitions == before && !kernelState);
                if (transition == 1) closeInput(nullptr, nullptr);
                else if (transition == 2) {
                    standbyResult = -EIO;
                    assert(standbyOutput(&second.common) == -EIO);
                    writeOnce(); assert(acquisitions == before && !kernelState);
                    standbyResult = 0;
                    assert(standbyOutput(&second.common) == 0);
                } else metadataOutput(&stream, &musicMetadata);
            }
            expireLeaseRetry();
            writeOnce(); writeOnce();
            assert(kernelState == (stereo ? 2u : 1u) && acquisitions == before + 1);
            assert(!outputs.at(&stream).preempted);
            assert(outputs.at(&stream).writtenFrames > acceptedBeforeRecovery);
            if (stereo) assert(receiverFirstFrame >= acceptedBeforeRecovery);
        }
    }

    // Recovery delays do not bypass calls, external routes, or live capture.
    for (unsigned guard = 0; guard < 6; ++guard) {
        reset(); enable("stereo"); kernelState = 0;
        writeOnce(); expireLeaseRetry();
        switch (guard) {
            case 0: phoneMode = AUDIO_MODE_IN_CALL; break;
            case 1: outputs.at(&stream).route = AUDIO_DEVICE_OUT_BLUETOOTH_A2DP; break;
            case 2: outputs.at(&stream).route = AUDIO_DEVICE_OUT_WIRED_HEADPHONE; break;
            case 3: inputs = 1; break;
            case 4: unmanagedOutputs.insert(&second); break;
            case 5: outputs.at(&stream).media = false; break;
        }
        const auto attempts = leaseAttempts;
        writeOnce(); writeOnce();
        assert(!kernelState && leaseAttempts == attempts && !receiver.state().open);
    }
    // Only a verified OFF state is recoverable. Read errors and unexpected
    // states retain the existing explicit-recovery requirement.
    for (bool readFailure : {false, true}) {
        reset(); enable("stereo");
        if (readFailure) kernelStateReadError = EIO;
        else kernelState = 3;
        writeOnce();
        assert(outputs.at(&stream).preempted && !kernelState && lease < 0);
        kernelStateReadError = 0;
        expireLeaseRetry(); writeOnce(); writeOnce();
        assert(acquisitions == 1 && !kernelState);
    }
    // The fresh kernel preflight may still find another route using the codec.
    reset(); enable("stereo"); kernelState = 0; writeOnce();
    rejectAcquire = true; acquireError = EBUSY;
    expireLeaseRetry(); writeOnce();
    assert(!kernelState && acquisitions == 1 && !outputs.at(&stream).preempted);
    rejectAcquire = false;
    writeOnce(); assert(!kernelState);
    expireLeaseRetry(); writeOnce(); assert(kernelState == 2 && acquisitions == 2);
}
static void testMultipleMediaMetadata() {
    for (bool v7 : {false, true}) for (const char* mode : {"dual_mono", "stereo"}) {
        reset(); enable(mode);
        std::array<playback_track_metadata, 2> tracks{{
            {AUDIO_USAGE_MEDIA, AUDIO_CONTENT_TYPE_MUSIC, 1.f},
            {AUDIO_USAGE_GAME, AUDIO_CONTENT_TYPE_MUSIC, 1.f}}};
        std::array<tetris::audio::LegacyPlaybackTrackMetadataV7, 2> tracksV7{};
        source_metadata metadata{tracks.size(), tracks.data()};
        source_metadata_v7 metadataV7{tracksV7.size(),
            reinterpret_cast<playback_track_metadata_v7*>(tracksV7.data())};
        auto update = [&] {
            if (v7) {
                tracksV7[0].base = tracks[0]; tracksV7[1].base = tracks[1];
                metadataOutputV7(&stream, &metadataV7);
            } else metadataOutput(&stream, &metadata);
        };
        const unsigned expected = kernelState, starts = receiverStarts;
        update(); writeOnce();
        assert(outputs.at(&stream).media && kernelState == expected && receiverStarts == starts);
        // The second record must be checked using the vendor's shorter stride.
        tracks[1].usage = AUDIO_USAGE_NOTIFICATION;
        update(); writeOnce();
        assert(!outputs.at(&stream).media && !kernelState && !receiver.state().open);
        tracks[1].usage = AUDIO_USAGE_MEDIA;
        update(); writeOnce(); writeOnce();
        assert(outputs.at(&stream).media && kernelState == expected);
    }
}
static void testPlaybackDiagnostics() {
    reset(); statusContains("tetris_audio_playback_gate=no_write");
    statusContains("tetris_audio_split_open=0");
    statusContains("tetris_audio_last_preempt=none");
    statusContains("tetris_audio_preemptions=0");
    reset(); rejectAcquire = true; authorizedMode = "dual_mono";
    assert(setParameters(nullptr, "tetris_audio_mode=dual_mono") == 0);
    writeOnce(); writeOnce();
    assert(!outputs.at(&stream).preempted && preemptionCount == 1);
    statusContains("tetris_audio_last_preempt=kernel_acquire");
    statusContains("tetris_audio_last_preempt_error=-16");
    rejectAcquire = false;
    writeOnce(); writeOnce();
    assert(preemptionCount == 1 && !acquisitions && !kernelState);
    statusContains("tetris_audio_last_preempt=kernel_acquire");
    statusContains("tetris_audio_last_preempt_error=-16");
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        reset(); requested = Mode::DualMono; preferenceLoaded = true;
        const char* expected = nullptr;
        switch (scenario) {
            case 0: phoneMode = AUDIO_MODE_IN_CALL; expected = "call"; break;
            case 1: outputs.at(&stream).route = AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
                    expected = "route"; break;
            case 2: currentFormat = AUDIO_FORMAT_PCM_FLOAT; expected = "format"; break;
            case 3: outputs.at(&stream).media = false; expected = "metadata"; break;
            case 4: inputs = 1; expected = "capture_or_unmanaged"; break;
            case 5: outputs.at(&stream).preempted = true; expected = "preempted"; break;
        }
        writeOnce();
        const auto field = std::string("tetris_audio_playback_gate=") + expected;
        statusContains(field.c_str());
    }
    reset();
    constexpr auto maximum = std::numeric_limits<uint64_t>::max();
    wideDiagnosticCounters = true;
    hookCalls = maximum;
    inputCounts = {maximum, maximum, maximum, maximum, maximum};
    lastPreemption = "kernel_state_changed";
    lastPreemptionError = std::numeric_limits<int>::min();
    preemptionCount = maximum;
    statusContains("tetris_audio_diagnostic_version=12");
}
static int routePatch(audio_stream_out* target, audio_devices_t route,
                      audio_patch_handle_t patchId) {
    audio_port_config source{}, sink{};
    source.type = AUDIO_PORT_TYPE_MIX;
    source.ext.mix.handle = outputs.at(target).handle;
    sink.type = AUDIO_PORT_TYPE_DEVICE;
    sink.ext.device.type = route;
    nextPatchHandle = patchId;
    audio_patch_handle_t handle = 0;
    return createPatch(nullptr, 1, &source, 1, &sink, &handle);
}
static void testBluetoothReturn() {
    constexpr audio_devices_t bluetooth = AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
    // The old Bluetooth patch can be released before or after its replacement.
    for (bool releaseFirst : {false, true}) {
        reset(); enable("stereo");
        assert(routePatch(&stream, bluetooth, 11) == 0);
        writeOnce(); assert(kernelState == 0);
        if (releaseFirst) assert(releasePatch(nullptr, 11) == 0);
        assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 12) == 0);
        if (!releaseFirst) assert(releasePatch(nullptr, 11) == 0);
        assert(outputs.at(&stream).route == AUDIO_DEVICE_OUT_SPEAKER);
        assert(outputs.at(&stream).routePatch == 12);
        writeOnce(); assert(kernelState == 0);
        writeOnce(); assert(kernelState == 2 && !outputs.at(&stream).preempted);
    }
    // A successful legacy routing parameter also supersedes the old patch.
    reset(); enable(); assert(routePatch(&stream, bluetooth, 11) == 0);
    assert(setOutputParameters(&stream.common, "routing=2") == 0);
    assert(releasePatch(nullptr, 11) == 0);
    assert(outputs.at(&stream).route == AUDIO_DEVICE_OUT_SPEAKER);
    writeOnce(); writeOnce(); assert(kernelState == 1);

    reset(); enable("stereo"); assert(routePatch(&stream, bluetooth, 11) == 0);
    patchResult = -EIO;
    assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 12) == -EIO);
    assert(outputs.at(&stream).route == bluetooth && outputs.at(&stream).routePatch == 11);
    writeOnce(); assert(kernelState == 0);
    patchResult = 0;
    assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 12) == 0);
    writeOnce(); writeOnce(); assert(kernelState == 2);

    releasePatchResult = -EIO;
    assert(releasePatch(nullptr, 12) == -EIO);
    assert(outputs.at(&stream).routePatch == 12 && patches.contains(12));
    assert(outputs.at(&stream).active && outputs.at(&stream).preempted);
    writeOnce(); assert(kernelState == 0);
    releasePatchResult = 0;
    assert(releasePatch(nullptr, 12) == 0);
    assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 13) == 0);
    writeOnce(); writeOnce(); assert(kernelState == 2);

    // Notification traffic and duplicate speaker routes do not cure an invalid
    // kernel state; a confirmed non-speaker -> speaker boundary permits a retry.
    reset(); enable("stereo"); kernelState = 3;
    writeOnce(); assert(outputs.at(&stream).preempted);
    const unsigned attempts = acquisitions;
    assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 12) == 0);
    assert(setOutputParameters(&stream.common, "routing=2") == 0);
    assert(setParameters(nullptr, "vendor_notification=1") == 0);
    writeOnce(); writeOnce();
    assert(acquisitions == attempts && outputs.at(&stream).preempted);
    assert(routePatch(&stream, bluetooth, 13) == 0);
    assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 14) == 0);
    outputs.at(&stream).media = false;
    writeOnce(); writeOnce(); assert(kernelState == 0);
    outputs.at(&stream).media = true;
    writeOnce(); assert(kernelState == 2);

    // A replacement stream remains blocked until the old Bluetooth output has
    // actually stopped. Removing its patch alone cannot prove that.
    for (bool closeOld : {false, true}) {
        reset(); enable("stereo"); assert(routePatch(&stream, bluetooth, 11) == 0);
        auto newOutput = outputs.at(&stream);
        newOutput.handle = 20; newOutput.route = AUDIO_DEVICE_NONE;
        newOutput.routePatch.reset(); newOutput.active = newOutput.primed = false;
        newOutput.writtenFrames = 0;
        outputs.emplace(&second, newOutput);
        assert(routePatch(&second, AUDIO_DEVICE_OUT_SPEAKER, 12) == 0);
        assert(releasePatch(nullptr, 11) == 0);
        writeOnce(&second); writeOnce(&second); assert(kernelState == 0);
        assert(outputs.at(&stream).active);
        if (closeOld) closeOutput(nullptr, &stream);
        else {
            standbyResult = -EIO;
            assert(standbyOutput(&stream.common) == -EIO);
            writeOnce(&second); assert(kernelState == 0);
            standbyResult = 0;
            assert(standbyOutput(&stream.common) == 0);
        }
        writeOnce(&second); writeOnce(&second); assert(kernelState == 2);
    }
}
static int control(const char* key, const char* value) {
    return setParameters(nullptr, (std::string(key) + "=" + value).c_str());
}
static void testSplitControls() {
    reset(); enable("stereo");
    const unsigned initialAcquisitions = acquisitions, initialGainWrites = gainUpdates;
    authorizedSoftwareGain = "6";
    assert(control("tetris_audio_software_gain", "6") == 0);
    assert(appliedSoftwareGain == 6 && softwareGainDb == 6 && kernelState == 2);
    assert(acquisitions == initialAcquisitions && gainUpdates == initialGainWrites);
    assert(control("tetris_audio_software_gain", "5") == -EPERM);
    for (const char* bad : {"-1", "7", "+6", "6x", "", "999999999999999999"})
        assert(control("tetris_audio_software_gain", bad) == -EINVAL);
    assert(setParameters(nullptr, "tetris_audio_software_gain=6;tetris_audio_mode=off") == -EINVAL);
    assert(setParameters(nullptr, "tetris_audio_output=upper;tetris_audio_output=lower") == -EINVAL);
    assert(kernelState == 2);
    statusContains("tetris_audio_software_gain_db=6");

    reset(); currentFormat = AUDIO_FORMAT_PCM_32_BIT; enable("stereo");
    assert(receiverDelayNs == 72000000);
    authorizedSyncTrim = "10";
    assert(control("tetris_audio_sync_trim", "10") == 0);
    writeOnce(); assert(receiverDelayNs == 72000000 && activeSyncTrimMs == 0);
    assert(standbyOutput(&stream.common) == 0);
    writeOnce(); writeOnce(); assert(receiverDelayNs == 82000000 && activeSyncTrimMs == 10);
    authorizedSyncTrim = "-10";
    assert(control("tetris_audio_sync_trim", "-10") == 0);
    writeOnce(); assert(receiverDelayNs == 82000000);
    assert(standbyOutput(&stream.common) == 0);
    writeOnce(); writeOnce(); assert(receiverDelayNs == 62000000);
    for (const char* bad : {"-11", "11", "", "1.0", "x"})
        assert(control("tetris_audio_sync_trim", bad) == -EINVAL);
    assert(control("tetris_audio_sync_trim", "0") == -EPERM);
    statusContains("tetris_audio_sync_trim_ms=-10");
    statusContains("tetris_audio_split_delay_ns=62000000");
    assert(standbyOutput(&stream.common) == 0);
    currentFormat = AUDIO_FORMAT_PCM_16_BIT;
    writeOnce(); writeOnce(); assert(receiverDelayNs == 0); // Unmeasured path unchanged.

    for (const char* selected : {"upper", "lower"}) {
        reset(); enable();
        authorizedOutput = selected;
        assert(control("tetris_audio_output", selected) == 0);
        assert(kernelState == 0 && active == Mode::Off);
        writeOnce(); assert(kernelState == 0);
        writeOnce();
        assert(kernelState == 2 && leaseMode == Mode::Stereo && active == Mode::DualMono);
        assert(lastSplitOutput == (std::string(selected) == "upper" ?
                SpeakerOutput::Upper : SpeakerOutput::Lower));
        statusContains("tetris_audio_active=dual_mono");
        statusContains((std::string("tetris_audio_output_active=") + selected).c_str());
        // Real route/call changes must forward the original full input.
        assert(routePatch(&stream, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, 20) == 0);
        const int16_t samples[]{100, -200, 300, -400};
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        assert(vendorBuffer == samples && kernelState == 0);
        statusContains("tetris_audio_output_active=both");
        statusContains("tetris_audio_active=off");
        assert(routePatch(&stream, AUDIO_DEVICE_OUT_SPEAKER, 21) == 0);
        writeOnce(); writeOnce(); assert(kernelState == 2);
        assert(setMode(nullptr, AUDIO_MODE_IN_CALL) == 0);
        assert(writeOutput(&stream, samples, sizeof(samples)) == sizeof(samples));
        assert(vendorBuffer == samples && kernelState == 0);
        assert(setMode(nullptr, AUDIO_MODE_NORMAL) == 0);
        authorizedOutput = "both";
        assert(control("tetris_audio_output", "both") == 0);
        writeOnce(); writeOnce(); assert(kernelState == 1);
    }
    reset();
    authorizedOutput = "upper";
    splitSupported = false;
    assert(control("tetris_audio_output", "upper") == -ENOTSUP);
    authorizedSoftwareGain = "1";
    assert(control("tetris_audio_software_gain", "1") == -ENOTSUP);
    authorizedSyncTrim = "1";
    assert(control("tetris_audio_sync_trim", "1") == -ENOTSUP);
    statusContains("tetris_audio_output_available=0");

    // Restore valid preferences once they appear, and contain invalid saved data.
    reset(); authorizedSoftwareGain = authorizedSyncTrim = authorizedOutput = "";
    restorePreference();
    assert(softwareGainDb == 0 && syncTrimMs == 0 && speakerOutput == SpeakerOutput::Both);
    authorizedSoftwareGain = "6"; authorizedSyncTrim = "-3"; authorizedOutput = "upper";
    restorePreference();
    assert(appliedSoftwareGain == 6 && syncTrimMs == -3 && speakerOutput == SpeakerOutput::Upper);
    const unsigned restoredGainUpdates = softwareGainUpdates;
    statusContains("tetris_audio_software_gain_db=6");
    statusContains("tetris_audio_output=upper");
    assert(softwareGainUpdates == restoredGainUpdates && !gainUpdates && !acquisitions);
    // A /data preference appearing after playback started tears down the old
    // backend before switching selection; it cannot leave an incompatible lease.
    reset(); authorizedOutput = ""; enable();
    assert(kernelState == 1 && !speakerOutputLoaded);
    authorizedOutput = "upper";
    writeOnce(); assert(kernelState == 0 && !outputs.at(&stream).preempted);
    writeOnce(); assert(kernelState == 2 && activeSpeakerOutput == SpeakerOutput::Upper);
    statusContains("tetris_audio_output_active=upper");
    reset(); authorizedSoftwareGain = "50"; authorizedSyncTrim = "50"; authorizedOutput = "bad";
    restorePreference();
    assert(appliedSoftwareGain == 0 && syncTrimMs == 0 && speakerOutput == SpeakerOutput::Both);
}
static void testReopenPreferences() {
    reset(); prepareDevice();
    authorizedMode = "stereo";
    authorizedSoftwareGain = "6"; authorizedSyncTrim = "10"; authorizedOutput = "upper";
    hw_device_t* opened = nullptr;
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(appliedSoftwareGain == 6 && syncTrimMs == 10 && speakerOutput == SpeakerOutput::Upper);
    assert(closeDevice(opened) == 0);
    // Reopen in the same process with new persisted values.
    prepareDevice();
    authorizedSoftwareGain = "2"; authorizedSyncTrim = "-8"; authorizedOutput = "lower";
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(appliedSoftwareGain == 2 && syncTrimMs == -8 && speakerOutput == SpeakerOutput::Lower);
    statusContains("tetris_audio_software_gain_db=2");
    statusContains("tetris_audio_sync_trim_ms=-8");
    statusContains("tetris_audio_output=lower");
    assert(closeDevice(opened) == 0);
    // Missing /data preferences must reset defaults, not retain old targets.
    prepareDevice();
    authorizedSoftwareGain = authorizedSyncTrim = authorizedOutput = "";
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(appliedSoftwareGain == 0 && softwareGainDb == 0 && syncTrimMs == 0 &&
            speakerOutput == SpeakerOutput::Both && activeSpeakerOutput == SpeakerOutput::Both);
    assert(!softwareGainLoaded && !syncTrimLoaded && !speakerOutputLoaded);
    statusContains("tetris_audio_software_gain_db=0");
    statusContains("tetris_audio_output=both");
    authorizedSoftwareGain = "4"; authorizedSyncTrim = "3"; authorizedOutput = "upper";
    statusContains("tetris_audio_software_gain_db=4");
    assert(appliedSoftwareGain == 4 && syncTrimMs == 3 && speakerOutput == SpeakerOutput::Upper);
    assert(closeDevice(opened) == 0);
}
static void testConsentGate() {
    // Previously loaded non-default controls cannot survive revoke/reset/accept.
    reset();
    authorizedSoftwareGain = "6"; authorizedSyncTrim = "-8";
    authorizedGain = "6"; authorizedOutput = "upper";
    enable("stereo");
    assert(softwareGainDb == 6 && syncTrimMs == -8 && speakerOutput == SpeakerOutput::Upper);
    acceptedConsent = false;
    authorizedMode = "off";
    authorizedSoftwareGain = authorizedSyncTrim = authorizedGain = "0";
    authorizedOutput = "both";
    assert(setParameters(nullptr, "tetris_audio_mode=off") == 0);
    statusContains("tetris_audio_active=off");
    assert(softwareGainDb == 0 && appliedSoftwareGain == 0 && syncTrimMs == 0 &&
            receiverGainDb == 0 && speakerOutput == SpeakerOutput::Both);
    acceptedConsent = true;
    enable("stereo");
    assert(softwareGainDb == 0 && appliedSoftwareGain == 0 && syncTrimMs == 0 &&
            receiverGainDb == 0 && speakerOutput == SpeakerOutput::Both);
    assert(lastSplitOutput == SpeakerOutput::Both);
    reset();
    acceptedConsent = false;
    authorizedMode = "dual_mono";
    restorePreference();
    assert(requested == Mode::Off && !preferenceLoaded);
    assert(setParameters(nullptr, "tetris_audio_mode=dual_mono") == -EPERM);
    writeOnce(); writeOnce();
    assert(kernelState == 0 && acquisitions == 0);
    acceptedConsent = true;
    writeOnce(); writeOnce();
    assert(requested == Mode::DualMono && kernelState != 0);
    acceptedConsent = false;
    writeOnce();
    assert(requested == Mode::Off && kernelState == 0);
    authorizedMode = "off";
    assert(setParameters(nullptr, "tetris_audio_mode=off") == 0);
}
int main() {
    if (const char* path = std::getenv("TETRIS_AUDIO_STATUS_FIXTURES")) {
        FILE* file = std::fopen(path, "w");
        assert(file && std::fclose(file) == 0);
    }
    testTransientLeaseRecovery();
    testStockRouteRevocationRecovery();
    testMultipleMediaMetadata();
    testConsentGate();
    testReopenPreferences();
    testBluetoothReturn();
    testSplitControls();
    testStereoSplit();
    testStereoStartupOrdering();
    testStartupClockRevalidation();
    testReceiverSegmentDrain();
    testPresentationClock();
    testReceiverCalibrationScope();
    testReceiverRevocation();
    testStereoAdmissionGuards();
    testIdleOutputDoesNotBlockPlayback();
    testAsynchronousPlaybackCallbacks();
    testQueuedPlaybackActivity();
    testUnsupportedMmapStart();
    testPlaybackDiagnostics();
    testInputScope();
    testVolumeCallbacks();
    testDolbyVolumeNotification();
    testReceiverGain();
    reset(); writeOnce(); writeOnce();
    assert(acquisitions == 0 && !lastBypass);
    reset(); enable(); assert(!lastBypass);
    events.clear(); assert(standbyOutput(&stream.common) == 0);
    assert((events == std::vector<std::string>{"restore", "standby"}));
    reset(); enable("stereo"); assert(!lastBypass);
    events.clear(); assert(setMode(nullptr, AUDIO_MODE_IN_CALL) == 0);
    assert((events == std::vector<std::string>{"restore", "mode"}));
    writeOnce(); writeOnce(); assert(kernelState == 0 && !lastBypass);
    modeResult = -EIO;
    assert(setMode(nullptr, AUDIO_MODE_NORMAL) == -EIO);
    writeOnce(); writeOnce(); assert(kernelState == 0 && !lastBypass);
    reset(); enable("stereo");
    events.clear(); assert(setOutputParameters(&stream.common, "routing=4") == 0);
    assert((events == std::vector<std::string>{"restore", "stream_parameters"}));
    writeOnce(); writeOnce(); assert(kernelState == 0);
    // Eligible metadata updates (including volume-only changes) do not discard
    // a target that has already moved to the vendor mixer worker.
    reset(); enable("stereo");
    const auto metadataGeneration = hookGeneration;
    playback_track_metadata mediaTrack{AUDIO_USAGE_MEDIA, AUDIO_CONTENT_TYPE_MUSIC, .5f};
    source_metadata mediaMetadata{1, &mediaTrack};
    metadataOutput(&stream, &mediaMetadata);
    assert(kernelState == 2 && hookGeneration == metadataGeneration);
    playback_track_metadata_v7 mediaTrackV7{};
    mediaTrackV7.base = mediaTrack;
    source_metadata_v7 mediaMetadataV7{1, &mediaTrackV7};
    metadataOutputV7(&stream, &mediaMetadataV7);
    observeHook = false;
    writeOnce();
    assert(kernelState == 2 && hookGeneration == metadataGeneration);
    assert(outputs.at(&stream).hookHandler == nullptr);
    reset(); enable();
    playback_track_metadata track{AUDIO_USAGE_VOICE_COMMUNICATION, AUDIO_CONTENT_TYPE_SPEECH, 1.f};
    source_metadata metadata{1, &track};
    metadataOutput(&stream, &metadata);
    assert(kernelState == 0); writeOnce(); assert(acquisitions == 1);
    reset(); enable();
    std::array<tetris::audio::LegacyPlaybackTrackMetadataV7, 2> mixedTracks{};
    mixedTracks[0].base.usage = AUDIO_USAGE_MEDIA;
    mixedTracks[1].base.usage = AUDIO_USAGE_VOICE_COMMUNICATION;
    source_metadata_v7 twoTrackMetadata{2,
        reinterpret_cast<playback_track_metadata_v7*>(mixedTracks.data())};
    metadataOutputV7(&stream, &twoTrackMetadata);
    assert(kernelState == 0 && !outputs.at(&stream).media);
    reset(); enable(); events.clear();
    assert(openInput(nullptr, 1, AUDIO_DEVICE_IN_BUILTIN_MIC, nullptr, nullptr,
                     AUDIO_INPUT_FLAG_NONE, "", AUDIO_SOURCE_MIC) == 0);
    assert((events == std::vector<std::string>{"restore", "input"}));
    writeOnce(); writeOnce(); assert(kernelState == 0);
    closeInput(nullptr, nullptr); writeOnce(); assert(kernelState == 1);
    reset(); enable("stereo"); kernelState = 0; // Stock mixer revoked the lease.
    writeOnce(); writeOnce(); assert(acquisitions == 1 && !lastBypass);
    expireLeaseRetry(); writeOnce(); assert(acquisitions == 2);
    for (unsigned gain : {208u, 65535u}) {
        reset(); kernelGain = gain; enable();
        statusContains("tetris_audio_available=1");
        reset(); kernelGain = gain; enable("stereo");
        assert(!lastBypass);
    }
    for (unsigned gain : {0u, 65536u}) {
        reset(); kernelGain = gain; authorizedMode = "dual_mono";
        assert(setParameters(nullptr, "tetris_audio_mode=dual_mono") == -ENOTSUP);
        assert(requested == Mode::Off && kernelState == 0);
        statusContains("tetris_audio_available=0");
    }
    reset(); kernelExists = false; authorizedMode = "dual_mono";
    assert(setParameters(nullptr, "tetris_audio_mode=dual_mono") == -ENOTSUP);
    statusContains("tetris_audio_reason=kernel_missing");
    reset(); enable();
    authorizedMode = "off";
    assert(setParameters(nullptr, "tetris_audio_mode=off") == 0);
    assert(kernelState == 0 && requested == Mode::Off);
    reset(); assert(setParameters(nullptr, "tetris_audio_mode=stereo;tetris_audio_mode=off") == -EINVAL);
    reset(); enable();
    Output another = outputs.at(&stream); another.active = false; another.primed = false;
    outputs.emplace(&second, another);
    writeOnce(&second); writeOnce(&stream); assert(kernelState == 0);
    reset(); enable("stereo"); writeResult = -EIO;
    assert(writeOutput(&stream, nullptr, 0) == -EIO); assert(kernelState == 0);
    assert(!outputs.at(&stream).hookHandler && !outputs.at(&stream).primed);
    writeResult = 1;
    writeOnce(); assert(kernelState == 0);
    writeOnce(); assert(kernelState == 2 && !lastBypass);
    reset(); enable(); events.clear();
    audio_port_config source{}; source.type = AUDIO_PORT_TYPE_MIX; source.ext.mix.handle = 10;
    audio_port_config sink{}; sink.type = AUDIO_PORT_TYPE_DEVICE; sink.ext.device.type = AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    audio_patch_handle_t patch = 0;
    assert(createPatch(nullptr, 1, &source, 1, &sink, &patch) == 0);
    assert((events == std::vector<std::string>{"restore", "patch"}));
    writeOnce(); writeOnce(); assert(kernelState == 0);
    reset(); enable(); events.clear();
    std::array<audio_port_config, 2> legacySources{};
    std::array<audio_port_config, 2> legacySinks{};
    setLegacyPortField(legacySources, 0, 8, AUDIO_PORT_TYPE_MIX);
    setLegacyPortField(legacySources, 0, 180, 20);
    setLegacyPortField(legacySources, 1, 8, AUDIO_PORT_TYPE_MIX);
    setLegacyPortField(legacySources, 1, 180, 10);
    setLegacyPortField(legacySinks, 0, 8, AUDIO_PORT_TYPE_DEVICE);
    setLegacyPortField(legacySinks, 0, 180, AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    Output secondRoute = outputs.at(&stream);
    secondRoute.handle = 20;
    outputs.emplace(&second, secondRoute);
    assert(createPatch(nullptr, 2, legacySources.data(), 1, legacySinks.data(), &patch) == 0);
    assert((events == std::vector<std::string>{"restore", "patch"}));
    assert(patchSourceCount == 2 && patchSources == legacySources.data());
    assert(patchSinkCount == 1 && patchSinks == legacySinks.data());
    assert(outputs.at(&stream).route == AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    assert(outputs.at(&second).route == AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    assert(releasePatch(nullptr, patch) == 0);
    assert(outputs.at(&stream).route == AUDIO_DEVICE_NONE);
    assert(outputs.at(&second).route == AUDIO_DEVICE_NONE);
    // A speaker plus another sink is never eligible, regardless of source order.
    setLegacyPortField(legacySinks, 0, 180, AUDIO_DEVICE_OUT_SPEAKER);
    setLegacyPortField(legacySinks, 1, 8, AUDIO_PORT_TYPE_DEVICE);
    setLegacyPortField(legacySinks, 1, 180, AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    assert(createPatch(nullptr, 2, legacySources.data(), 2, legacySinks.data(), &patch) == 0);
    assert(patchSinkCount == 2 && patchSinks == legacySinks.data());
    assert(outputs.at(&stream).route == AUDIO_DEVICE_NONE);
    assert(outputs.at(&second).route == AUDIO_DEVICE_NONE);
    writeOnce(); writeOnce(); assert(kernelState == 0);
    reset(); rejectAcquire = true; authorizedMode = "stereo";
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == 0);
    writeOnce(); writeOnce(); assert(kernelState == 0 && !lastBypass);
    reset();
    assert(setParameters(nullptr, "tetris_audio_mode=stereo") == -EPERM);
    assert(requested == Mode::Off && kernelState == 0);
    reset();
    blockWrite = true; blockEntered = false; releaseBlockedWrite = false; blockedTarget = &stream;
    std::thread stockWriter([] { writeOnce(); });
    {
        std::unique_lock lock(blockMutex);
        assert(blockCv.wait_for(lock, std::chrono::seconds(2), [] { return blockEntered; }));
    }
    authorizedMode = "dual_mono";
    // This must finish while the OFF vendor write is still blocked.
    assert(setParameters(nullptr, "tetris_audio_mode=dual_mono") == 0);
    assert(stockWrites == 1 && kernelState == 0);
    {
        std::lock_guard lock(blockMutex);
        releaseBlockedWrite = true;
    }
    blockCv.notify_all();
    stockWriter.join();
    blockWrite = false; blockedTarget = nullptr;
    assert(stockWrites == 0 && kernelState == 0);
    writeOnce(); assert(kernelState == 0);
    writeOnce(); assert(kernelState == 1);
    reset(); prepareDevice(); fakeDevice.common.version = AUDIO_DEVICE_API_VERSION_3_0;
    hw_device_t* opened = nullptr;
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(opened == &fakeDevice.common && fakeDevice.common.close == fakeDeviceClose);
    assert(fakeDevice.open_output_stream == fakeOpenOutput && device == nullptr);
    reset(); prepareDevice(); hookReady = false;
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(fakeDevice.get_parameters == fakeGetParameters && device == nullptr);
    reset(); prepareDevice(); fakeDevice.set_mode = nullptr;
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(fakeDevice.common.close == fakeDeviceClose && device == nullptr);
    reset(); prepareDevice(); authorizedMode = "stereo";
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(requested == Mode::Stereo && active == Mode::Off && kernelState == 0);
    assert(fakeDevice.common.close == closeDevice);
    audio_stream_out* openedStream = nullptr;
    unsupportedStream = true;
    assert(openOutput(&fakeDevice, 20, AUDIO_DEVICE_OUT_SPEAKER, AUDIO_OUTPUT_FLAG_DEEP_BUFFER,
                      nullptr, &openedStream, "") == 0);
    assert(openedStream->write == nullptr && unmanagedOutputs.count(openedStream) == 1);
    closeOutput(&fakeDevice, openedStream);
    assert(unmanagedOutputs.empty());
    assert(closeDevice(opened) == 0);
    assert(authorizedMode == "stereo" && requested == Mode::Off);
    reset(); prepareDevice(); authorizedMode = "dual_mono"; kernelExists = false;
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(requested == Mode::DualMono && active == Mode::Off && kernelState == 0);
    statusContains("tetris_audio_reason=kernel_missing");
    assert(closeDevice(opened) == 0);
    reset(); prepareDevice(); authorizedMode = "";
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(requested == Mode::Off && !preferenceLoaded);
    authorizedMode = "stereo";
    writeOnce();
    assert(requested == Mode::Stereo && preferenceLoaded && active == Mode::Off);
    writeOnce(); assert(active == Mode::Stereo && kernelState == 2);
    assert(closeDevice(opened) == 0);
    reset(); prepareDevice(); authorizedMode = "invalid";
    assert(openDevice(nullptr, AUDIO_HARDWARE_INTERFACE, &opened) == 0);
    assert(requested == Mode::Off && kernelState == 0);
    assert(closeDevice(opened) == 0);
    printf("Tetris HAL callback/lease checks passed (maximum status %zu bytes)\n", longestStatus);
}
