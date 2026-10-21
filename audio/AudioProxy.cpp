// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "TetrisAudio"
#include "LegacyAudioHal.h"
#include "AudioMode.h"
#include "PlaybackMetadata.h"
#include "DownmixHook.h"
#include "DspStereoHook.h"
#include "BessoundProbe.h"
#include "TetrisAudioAbi.h"
#include "StereoReceiver.h"

#include <android-base/properties.h>
#include <log/log.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <cerrno>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace {
using tetris::audio::Mode;
using tetris::audio::SpeakerOutput;
using tetris::audio::modeName;
using tetris::audio::parseMode;
constexpr char kKernelDevice[] = "/dev/tetris_audio";
constexpr char kVendorHal[] = "/vendor/lib64/hw/audio.primary.mediatek.so";
// The kernel reports the programmed 16-bit Q16 receiver gain.
constexpr unsigned kMaxReceiverGain = 0xffffU;
constexpr auto kLeaseRetryDelay = std::chrono::milliseconds(250);

// Vendor objects and all unmodified callbacks retain their original addresses.
// Serialize only callbacks that can change playback, routing or lease ownership.
std::recursive_mutex mutex;
void* vendorLibrary = nullptr;
audio_hw_device* device = nullptr;
Mode requested = Mode::Off;
Mode active = Mode::Off;
Mode leaseMode = Mode::Off;
SpeakerOutput speakerOutput = SpeakerOutput::Both;
SpeakerOutput activeSpeakerOutput = SpeakerOutput::Both;
int softwareGainDb = 0;
int syncTrimMs = 0, activeSyncTrimMs = 0;
bool softwareGainLoaded = false, syncTrimLoaded = false, speakerOutputLoaded = false;
bool preferenceLoaded = false;
bool gainPreferenceLoaded = false;
bool gainAvailable = false;
bool splitAvailable = false;
tetris::audio::StereoReceiver receiver;
uint64_t acceptedReceiverFrames = 0;
int64_t receiverSegmentEndNs = 0;
int64_t receiverDelayNs = 0;
int receiverGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
int reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
audio_mode_t phoneMode = AUDIO_MODE_NORMAL;
int lease = -1;
unsigned inputs = 0;
unsigned stockWrites = 0;
unsigned vendorWriteDepth = 0;
bool dspRevoking = false;
bool dspRevokePending = false;
tetris_audio_caps capabilities{};
const char* reason = "idle";
const char* playbackGate = "no_write";
// Retain the latest cause for diagnostics across playback and mode changes.
const char* lastPreemption = "none";
int lastPreemptionError = 0;
uint64_t preemptionCount = 0;
int leaseError = 0;
struct InputCounts {
    uint64_t buffers = 0, frames = 0, different = 0, leftNonzero = 0, rightNonzero = 0;
} inputCounts;

// Bounded counts of input offered to an authorized stereo write, never recordings.
template <typename Sample> void countInput(const void* buffer, size_t bytes) {
    constexpr size_t frameBytes = 2 * sizeof(Sample);
    if (!buffer || !bytes || bytes % frameBytes) return;
    const size_t frames = std::min<size_t>(bytes / frameBytes, 64);
    const auto* pcm = static_cast<const unsigned char*>(buffer);
    ++inputCounts.buffers;
    inputCounts.frames += frames;
    for (size_t frame = 0; frame < frames; ++frame) {
        Sample pair[2];
        std::memcpy(pair, pcm + frame * frameBytes, sizeof(pair));
        inputCounts.different += pair[0] != pair[1];
        inputCounts.leftNonzero += pair[0] != 0;
        inputCounts.rightNonzero += pair[1] != 0;
    }
}

struct DeviceCallbacks {
    decltype(audio_hw_device::common.close) close = nullptr;
    decltype(audio_hw_device::set_mode) setMode = nullptr;
    decltype(audio_hw_device::set_parameters) setParameters = nullptr;
    decltype(audio_hw_device::get_parameters) getParameters = nullptr;
    decltype(audio_hw_device::open_output_stream) openOutput = nullptr;
    decltype(audio_hw_device::close_output_stream) closeOutput = nullptr;
    decltype(audio_hw_device::open_input_stream) openInput = nullptr;
    decltype(audio_hw_device::close_input_stream) closeInput = nullptr;
    decltype(audio_hw_device::create_audio_patch) createPatch = nullptr;
    decltype(audio_hw_device::release_audio_patch) releasePatch = nullptr;
    decltype(audio_hw_device::set_audio_port_config) setPort = nullptr;
} original;
struct Output {
    audio_io_handle_t handle;
    audio_devices_t route;
    audio_output_flags_t flags;
    bool active = false;
    bool media = false;
    bool primed = false;
    bool preempted = false;
    void* hookHandler = nullptr;
    bool hookAmbiguous = false;
    decltype(audio_stream_out::write) write;
    decltype(audio_stream::standby) standby;
    decltype(audio_stream::set_parameters) setParameters;
    decltype(audio_stream_out::update_source_metadata) metadata;
    decltype(audio_stream_out::update_source_metadata_v7) metadataV7;
    bool dspAttempted = false;
    decltype(audio_stream_out::start) start = nullptr;
    decltype(audio_stream_out::resume) resume = nullptr;
    decltype(audio_stream_out::stop) stop = nullptr;
    const char* preemptionReason = nullptr;
    // The vendor counter is cumulative from open, including across standby.
    uint64_t writtenFrames = 0;
    bool frameAccountingValid = true;
    // Releasing an older patch must not erase a newer route on this stream.
    std::optional<audio_patch_handle_t> routePatch = {};
    std::chrono::steady_clock::time_point leaseRetryAfter{};
};
uint64_t hookGeneration = 0;
std::map<audio_stream_out*, Output> outputs;
std::set<audio_stream_out*> unmanagedOutputs;
std::map<audio_patch_handle_t, std::vector<audio_io_handle_t>> patches;

void confirmOutputRoute(Output& output, audio_devices_t route,
                        std::optional<audio_patch_handle_t> patch = {}) {
    const bool returnedToSpeaker = output.route != AUDIO_DEVICE_OUT_SPEAKER &&
            route == AUDIO_DEVICE_OUT_SPEAKER;
    output.route = route;
    output.routePatch = patch;
    if (returnedToSpeaker) {
        // A completed route transition permits a new attempt. Media metadata,
        // exclusive playback and a fresh priming write are still required.
        output.preempted = false;
        output.preemptionReason = nullptr;
        output.leaseRetryAfter = {};
    }
}

std::optional<int> parseReceiverGain(std::string_view value) {
    if (value.empty()) return {};
    int gain = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), gain);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            gain < TETRIS_AUDIO_RECEIVER_MIN_DB || gain > TETRIS_AUDIO_RECEIVER_MAX_DB) return {};
    return gain;
}
std::optional<int> parseBoundedInteger(std::string_view value, int minimum, int maximum) {
    if (value.empty()) return {};
    int number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            number < minimum || number > maximum) return {};
    return number;
}
std::optional<SpeakerOutput> parseSpeakerOutput(std::string_view value) {
    if (value == "both") return SpeakerOutput::Both;
    if (value == "upper") return SpeakerOutput::Upper;
    if (value == "lower") return SpeakerOutput::Lower;
    return {};
}
const char* speakerOutputName(SpeakerOutput output) {
    switch (output) {
        case SpeakerOutput::Upper: return "upper";
        case SpeakerOutput::Lower: return "lower";
        case SpeakerOutput::Both: return "both";
    }
    return "both";
}
Mode requestedBackend() {
    return requested != Mode::Off && speakerOutput != SpeakerOutput::Both ? Mode::Stereo : requested;
}
bool invalidatePlayback(const char* why, bool resetPreemption);
bool consentGranted() {
    return android::base::GetProperty("sys.tetrisparts.consent", "0") == "1";
}
void restorePreference() {
    // Restore saved audio only after Parts republishes consent for the current
    // terms. A saved preference alone cannot authorize receiver routing.
    if (!consentGranted()) {
        requested = Mode::Off;
        preferenceLoaded = gainPreferenceLoaded = false;
        softwareGainLoaded = syncTrimLoaded = speakerOutputLoaded = false;
        if (lease >= 0) invalidatePlayback("idle", false);
        // Clear loaded runtime values too: agreeing later must not resurrect an
        // old gain, route or trim before the next persisted-preference read.
        if (softwareGainDb != 0) receiver.setSoftwareGain(0);
        softwareGainDb = syncTrimMs = 0;
        speakerOutput = SpeakerOutput::Both;
        receiverGainDb = reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
        return;
    }
    // Defaults apply before /data properties are available; retry a missing
    // property on later writes rather than permanently losing boot preferences.
    if (!softwareGainLoaded) {
        const auto saved = android::base::GetProperty("persist.sys.tetris.audio_software_gain", "");
        if (!saved.empty()) {
            softwareGainDb = parseBoundedInteger(saved, 0, 6).value_or(0);
            receiver.setSoftwareGain(softwareGainDb);
            softwareGainLoaded = true;
        }
    }
    if (!syncTrimLoaded) {
        const auto saved = android::base::GetProperty("persist.sys.tetris.audio_sync_trim", "");
        if (!saved.empty()) {
            syncTrimMs = parseBoundedInteger(saved, -10, 10).value_or(0);
            syncTrimLoaded = true;
        }
    }
    if (!speakerOutputLoaded) {
        const auto saved = android::base::GetProperty("persist.sys.tetris.audio_output", "");
        if (!saved.empty()) {
            const auto restored = parseSpeakerOutput(saved).value_or(SpeakerOutput::Both);
            if (restored == speakerOutput || invalidatePlayback("idle", false)) {
                speakerOutput = restored;
                speakerOutputLoaded = true;
            }
        }
    }
    // /data properties may not be loaded during HAL startup.
    if (!preferenceLoaded) {
        const std::string saved = android::base::GetProperty("persist.sys.tetris.audio_mode", "");
        if (!saved.empty()) {
            requested = parseMode(saved).value_or(Mode::Off);
            preferenceLoaded = true;
        }
    }
    if (!gainPreferenceLoaded) {
        const std::string saved = android::base::GetProperty("persist.sys.tetris.audio_gain", "");
        if (!saved.empty()) {
            // Earlier builds allowed +8 dB. Restore that preference at the new
            // ceiling without accepting out-of-range live requests.
            receiverGainDb = (saved == "7" || saved == "8") ? TETRIS_AUDIO_RECEIVER_MAX_DB :
                    parseReceiverGain(saved).value_or(TETRIS_AUDIO_RECEIVER_DEFAULT_DB);
            gainPreferenceLoaded = true;
        }
    }
}

void releaseLease(const char* why) {
    receiver.close();
    acceptedReceiverFrames = 0;
    receiverSegmentEndNs = 0;
    receiverDelayNs = 0;
    tetris_audio_hook_clear();
    hookGeneration = 0;
    if (lease >= 0) {
        // Kernel release performs the complete restoration, including process death.
        close(lease);
        lease = -1;
    }
    active = leaseMode = Mode::Off;
    activeSpeakerOutput = SpeakerOutput::Both;
    activeSyncTrimMs = 0;
    reason = why;
}
// Restore the shared DSP task before another route can inherit its configuration.
// Called only outside vendor writes, with the proxy mutex held and no hook lock.
bool restoreDspOwner() {
    const auto dsp = tetris_dsp_stereo_state();
    if (!dsp.dirty) return true;
    if (vendorWriteDepth || dspRevoking) {
        reason = "dsp_recovery_required";
        return false;
    }
    tetris_dsp_stereo_revoke();
    const auto found = outputs.find(static_cast<audio_stream_out*>(dsp.owner));
    if (found == outputs.end()) {
        tetris_dsp_stereo_fault();
        reason = "dsp_recovery_required";
        return false;
    }
    dspRevoking = true;
    const int result = found->second.standby(&found->first->common);
    dspRevoking = false;
    auto& output = found->second;
    output.active = output.primed = false;
    output.hookHandler = nullptr;
    output.hookAmbiguous = false;
    if (result != 0 || tetris_dsp_stereo_state().dirty) {
        tetris_dsp_stereo_fault();
        reason = "dsp_recovery_required";
        ALOGE("Experimental DSP task did not complete teardown (%d)", result);
        return false;
    }
    return true;
}
bool revokePlayback(const char* why) {
    if (vendorWriteDepth) {
        dspRevokePending = true;
        tetris_audio_hook_clear();
        tetris_dsp_stereo_revoke();
        return false;
    }
    if (dspRevoking) return false;
    releaseLease(why);
    return restoreDspOwner();
}
void recordPreemption(const char* why, int error) {
    lastPreemption = why;
    lastPreemptionError = error;
    ++preemptionCount;
}
void preemptOutput(Output& output, const char* why, int error = 0,
                   const char* status = nullptr) {
    output.preempted = true;
    output.preemptionReason = status;
    output.leaseRetryAfter = {};
    recordPreemption(why, error);
}
void preemptAll(const char* why, int error = 0, const char* status = nullptr) {
    for (auto& [stream, output] : outputs) {
        (void)stream;
        preemptOutput(output, why, error, status);
    }
}
void preemptPlaying(const char* why, int error = 0, audio_stream_out* current = nullptr,
                    const char* status = nullptr) {
    const auto owner = tetris_dsp_stereo_state().owner;
    for (auto& [stream, output] : outputs) {
        if (output.active || stream == owner || stream == current) preemptOutput(output, why, error, status);
    }
}
void resetHookTargets() {
    for (auto& [stream, output] : outputs) {
        (void)stream;
        output.hookHandler = nullptr;
        output.hookAmbiguous = false;
        output.primed = false;
        output.dspAttempted = false;
    }
}
bool invalidatePlayback(const char* why, bool resetPreemption = true) {
    if (!revokePlayback(why)) return false;
    resetHookTargets();
    for (auto& [stream, output] : outputs) {
        (void)stream;
        // Changing parameters does not itself stop a vendor stream. Keep
        // activity until write/standby/stop/close confirms its actual lifetime.
        output.primed = false;
        if (resetPreemption) {
            output.preempted = false;
            output.leaseRetryAfter = {};
        }
    }
    return true;
}
bool validCapabilities(const tetris_audio_caps& caps) {
    return caps.version == 1 && caps.reserved == 0 &&
            caps.receiver_gain > 0 && caps.receiver_gain <= kMaxReceiverGain &&
            (caps.modes & (1u << static_cast<unsigned>(Mode::DualMono)));
}
bool refreshCapabilities() {
    splitAvailable = false;
    gainAvailable = false;
    reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    int fd = lease;
    if (fd < 0) fd = open(kKernelDevice, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        capabilities = {};
        reason = "kernel_missing";
        return false;
    }
    tetris_audio_caps caps{};
    bool ok = ioctl(fd, TETRIS_AUDIO_GET_CAPS, &caps) == 0 && validCapabilities(caps);
    __s32 gain = 0;
    if (ok && ioctl(fd, TETRIS_AUDIO_GET_GAIN, &gain) == 0 &&
            gain >= TETRIS_AUDIO_RECEIVER_MIN_DB && gain <= TETRIS_AUDIO_RECEIVER_MAX_DB) {
        gainAvailable = true;
        reportedGainDb = fd == lease ? gain : receiverGainDb;
    }
    tetris_audio_split_caps split{};
    splitAvailable = ok && ioctl(fd, TETRIS_AUDIO_GET_SPLIT_CAPS, &split) == 0 &&
            split.version == TETRIS_AUDIO_SPLIT_VERSION && split.rate == 48000 &&
            split.channels == 1 && split.format == TETRIS_AUDIO_SPLIT_FORMAT_S32_LE &&
            split.reserved[0] == 0 && split.reserved[1] == 0;
    if (fd != lease) close(fd);
    capabilities = ok ? caps : tetris_audio_caps{};
    if (!ok) reason = "calibration_missing";
    return ok;
}
bool checkLease() {
    const auto receiverState = receiver.state();
    if (leaseMode == Mode::Stereo && receiverState.failed) {
        timespec now{};
        // A natural pause can drain the short receiver FIFO before the vendor
        // calls standby. Resume with a new clock, without replaying old samples.
        const bool drained = receiverState.error == EPIPE && acceptedReceiverFrames &&
                receiverState.writtenFrames == acceptedReceiverFrames &&
                clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                int64_t(now.tv_sec) * 1000000000LL + now.tv_nsec >= receiverSegmentEndNs;
        if (!drained) preemptPlaying("receiver_clock", -receiverState.error, nullptr, "sync_error");
        if (!revokePlayback(drained ? "idle" : "sync_error")) return false;
        resetHookTargets();
    }
    if (lease < 0) return !tetris_dsp_stereo_state().dirty ||
            tetris_dsp_stereo_state().ready || restoreDspOwner();
    __u32 state = 0;
    const int result = ioctl(lease, TETRIS_AUDIO_GET_STATE, &state);
    const int error = result != 0 ? -errno : 0;
    if (result != 0 ||
            state != static_cast<unsigned>(leaseMode)) {
        // Teardown clears the DSP owner's active flag. Mark affected streams
        // first; a dormant output never owned the lost route.
        preemptPlaying(result != 0 ? "kernel_state_read" : "kernel_state_changed", error);
        if (!revokePlayback("route")) return false;
        resetHookTargets();
    }
    return true;
}
bool acquireLease(Mode mode) {
    leaseError = 0;
    if (lease >= 0) return leaseMode == mode;
    lease = open(kKernelDevice, O_RDWR | O_CLOEXEC);
    if (lease < 0) { leaseError = -errno; reason = "kernel_missing"; return false; }
    tetris_audio_caps caps{};
    const int capsResult = ioctl(lease, TETRIS_AUDIO_GET_CAPS, &caps);
    if (capsResult != 0 || !validCapabilities(caps) ||
            !(caps.modes & (1u << static_cast<unsigned>(mode)))) {
        leaseError = capsResult != 0 ? -errno : -ENOTSUP;
        releaseLease("calibration_missing");
        return false;
    }
    __s32 gain = receiverGainDb;
    if (ioctl(lease, TETRIS_AUDIO_SET_GAIN, &gain) != 0) {
        if (errno != ENOTTY) {
            leaseError = -errno;
            releaseLease("gain_error");
            return false;
        }
        // ABI v1 kernels predating gain control keep their original media gain.
        gainAvailable = false;
        reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    } else {
        gainAvailable = true;
        reportedGainDb = gain;
    }
    if (mode == Mode::Stereo && !receiver.start(lease)) {
        leaseError = -receiver.state().error;
        releaseLease("receiver_unavailable");
        return false;
    }
    __u32 value = static_cast<unsigned>(mode);
    if (ioctl(lease, TETRIS_AUDIO_SET_MODE, &value) != 0) {
        leaseError = -errno;
        releaseLease("route");
        return false;
    }
    // DL8 is a dynamic DPCM frontend: its backend becomes reachable only
    // after the lease connects the stereo route. Preparing before SET_MODE
    // would fail even though the bound PCM is correctly configured.
    if (mode == Mode::Stereo && !receiver.prepare()) {
        leaseError = -receiver.state().error;
        releaseLease("receiver_unavailable");
        return false;
    }
    capabilities = caps;
    leaseMode = mode;
    active = requested;
    activeSpeakerOutput = speakerOutput;
    activeSyncTrimMs = syncTrimMs;
    reason = "playing";
    return true;
}
bool pcmEligible(const audio_stream_out* stream, const Output& output) {
    constexpr unsigned unsupported = AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD |
            AUDIO_OUTPUT_FLAG_MMAP_NOIRQ | AUDIO_OUTPUT_FLAG_HW_AV_SYNC;
    if (static_cast<unsigned>(output.flags) & unsupported) return false;
    if (stream->common.get_channels(&stream->common) != AUDIO_CHANNEL_OUT_STEREO) return false;
    const auto format = stream->common.get_format(&stream->common);
    return format == AUDIO_FORMAT_PCM_16_BIT || format == AUDIO_FORMAT_PCM_32_BIT;
}
size_t pcmFrameBytes(const audio_stream_out* stream) {
    if (!stream->common.get_channels || !stream->common.get_format ||
            stream->common.get_channels(&stream->common) != AUDIO_CHANNEL_OUT_STEREO) return 0;
    switch (stream->common.get_format(&stream->common)) {
        case AUDIO_FORMAT_PCM_16_BIT: return 4;
        case AUDIO_FORMAT_PCM_32_BIT: return 8;
        default: return 0;
    }
}
void accountWrite(Output& output, ssize_t result, size_t offered, size_t frameBytes) {
    if (result <= 0) return;
    output.active = true;
    if (!frameBytes || size_t(result) > offered || size_t(result) % frameBytes ||
            uint64_t(result) / frameBytes > UINT64_MAX - output.writtenFrames) {
        output.frameAccountingValid = false;
        return;
    }
    output.writtenFrames += size_t(result) / frameBytes;
}
int64_t receiverDelay(const audio_stream_out* stream, const Output& output) {
    // Acoustic calibration of Tetris's 48 kHz PCM32 deep-buffer speaker path:
    // the lower speaker trails the receiver by 72 ms with uncorrected targets.
    // Leave other output paths and the framework's presentation clock unchanged.
    return output.flags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER &&
            stream->common.get_sample_rate &&
            stream->common.get_sample_rate(&stream->common) == 48000 &&
            stream->common.get_format &&
            stream->common.get_format(&stream->common) == AUDIO_FORMAT_PCM_32_BIT ?
            tetris::audio::kDeepBufferReceiverDelayNs +
                    int64_t(lease >= 0 ? activeSyncTrimMs : syncTrimMs) * 1000000 : 0;
}
std::optional<int64_t> presentationTarget(const audio_stream_out* stream, const Output& output) {
    if (!output.frameAccountingValid || !stream->get_presentation_position ||
            !stream->common.get_sample_rate ||
            stream->common.get_sample_rate(&stream->common) != 48000) return {};
    uint64_t frames = 0;
    timespec stamp{}, now{};
    if (stream->get_presentation_position(stream, &frames, &stamp) != 0 ||
            stamp.tv_sec < 0 || stamp.tv_nsec < 0 || stamp.tv_nsec >= 1000000000LL ||
            stamp.tv_sec > (INT64_MAX - stamp.tv_nsec) / 1000000000LL ||
            clock_gettime(CLOCK_MONOTONIC, &now) != 0) return {};
    return tetris::audio::receiverTargetTime(
            {frames, int64_t(stamp.tv_sec) * 1000000000LL + stamp.tv_nsec},
            output.writtenFrames, int64_t(now.tv_sec) * 1000000000LL + now.tv_nsec,
            receiverDelay(stream, output));
}
ssize_t writeOutput(audio_stream_out* stream, const void* buffer, size_t bytes) {
    std::unique_lock lock(mutex);
    auto found = outputs.find(stream);
    if (found == outputs.end()) return -ENODEV;
    auto& output = found->second;
    restorePreference();
    if (dspRevoking || vendorWriteDepth) return -EIO;
    const size_t frameBytes = pcmFrameBytes(stream);
    if (requested == Mode::Off) {
        playbackGate = "disabled";
        if (!revokePlayback("idle")) return -EIO;
        auto function = output.write;
        ++stockWrites;
        lock.unlock();
        const ssize_t result = function(stream, buffer, bytes);
        lock.lock();
        --stockWrites;
        found = outputs.find(stream);
        if (found != outputs.end()) {
            accountWrite(found->second, result, bytes, frameBytes);
            found->second.primed = false;
            found->second.hookHandler = nullptr;
            found->second.hookAmbiguous = false;
            found->second.dspAttempted = false;
        }
        return result;
    }
    if (!checkLease()) return -EIO;
    bool exclusive = true;
    for (const auto& [otherStream, other] : outputs) {
        if (otherStream != stream && other.active) exclusive = false;
    }
    tetris::audio::PlaybackState state{
        phoneMode == AUDIO_MODE_NORMAL, output.route == AUDIO_DEVICE_OUT_SPEAKER,
        pcmEligible(stream, output), output.media, exclusive,
        inputs != 0 || stockWrites != 0 || !unmanagedOutputs.empty(),
        output.preempted, output.primed,
    };
    playbackGate = !state.normalMode ? "call" : !state.speakerOnly ? "route" :
            !state.pcmStereo ? "format" : !state.mediaOnly ? "metadata" :
            !state.exclusiveOutput ? "other_output" : state.captureOpen ? "capture_or_unmanaged" :
            state.preempted ? "preempted" : "eligible";
    bool canEnable = tetris::audio::eligible(state);
    std::optional<int64_t> target;
    tetris::audio::StereoBlock block;
    const char* why = phoneMode != AUDIO_MODE_NORMAL ? "call" :
            output.preempted ? (output.preemptionReason ? output.preemptionReason : "route") : "idle";
    if (canEnable && output.leaseRetryAfter != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() < output.leaseRetryAfter) {
        canEnable = false;
        playbackGate = "lease_busy";
        why = "route";
    }
    if (canEnable && requestedBackend() == Mode::Stereo) {
        if (!stream->common.get_sample_rate ||
                stream->common.get_sample_rate(&stream->common) != 48000) {
            canEnable = false;
            why = "stereo_format";
        } else {
            target = presentationTarget(stream, output);
            if (!target) {
                if (leaseMode == Mode::Stereo) preemptOutput(output, "presentation_clock", -EPROTO,
                                                        "sync_error");
                canEnable = false;
                why = output.preempted ? "sync_error" : "sync_waiting";
            } else if (!tetris::audio::splitStereo(buffer, bytes, frameBytes * 4, &block, speakerOutput)) {
                canEnable = false;
                why = "stereo_format";
            }
        }
    }
    const bool startingStereo = canEnable && requestedBackend() == Mode::Stereo && lease < 0;
    if (!canEnable) {
        if (!revokePlayback(why)) return -EIO;
    } else if (!acquireLease(requestedBackend())) {
        // Route transitions may briefly hold the receiver. The failed attempt
        // has released its resources; retry only from a later eligible write,
        // with a fresh clock and the kernel's full routing checks. Actual route
        // loss and receiver faults still use the permanent preemption path.
        if (leaseError == -EBUSY || leaseError == -EAGAIN) {
            output.leaseRetryAfter = std::chrono::steady_clock::now() + kLeaseRetryDelay;
            playbackGate = "lease_busy";
            reason = "route";
            recordPreemption("kernel_acquire", leaseError);
        } else {
            preemptOutput(output, "kernel_acquire", leaseError, reason);
        }
        if (!restoreDspOwner()) return -EIO;
    } else {
        output.leaseRetryAfter = {};
    }
    if (startingStereo && leaseMode == Mode::Stereo) {
        // Opening and preparing the receiver can outlive the first clock
        // sample. Check again before the main output consumes only the right
        // channel; a missing clock must keep this whole block on stock audio.
        target = presentationTarget(stream, output);
        if (!target) {
            canEnable = false;
            releaseLease("sync_waiting");
        }
    }
    const bool split = canEnable && leaseMode == Mode::Stereo && target.has_value();
    const uint64_t firstFrame = output.writtenFrames;
    if (split) {
        if (frameBytes == 4) countInput<int16_t>(buffer, bytes);
        else countInput<int32_t>(buffer, bytes);
    }
    // Keep the vendor's protected SmartPA processing on the bottom speaker.
    // Its mono DSP only sees the right channel; left has a separate PCM route.
    TetrisHookScope observation{};
    tetris_audio_hook_begin(&observation, nullptr, 0);
    ++vendorWriteDepth;
    const ssize_t result = output.write(stream, split ? block.right.data() : buffer, bytes);
    --vendorWriteDepth;
    tetris_audio_hook_end(&observation);
    accountWrite(output, result, bytes, frameBytes);
    output.primed = result > 0;
    if (dspRevokePending) {
        dspRevokePending = false;
        preemptPlaying("deferred_revoke", 0, stream);
        revokePlayback("route");
        output.primed = false;
        // Never replay bytes already accepted by the main output.
        return result;
    }
    if (split && result > 0) {
        if (!output.frameAccountingValid || !receiver.enqueue(std::move(block.left),
                size_t(result) / frameBytes, firstFrame, *target)) {
            preemptOutput(output, "receiver_write", output.frameAccountingValid ?
                    -receiver.state().error : -EPROTO, "sync_error");
            revokePlayback("sync_error");
        } else {
            const uint64_t accepted = size_t(result) / frameBytes;
            receiverDelayNs = receiverDelay(stream, output);
            acceptedReceiverFrames += accepted;
            receiverSegmentEndNs = *target + int64_t(accepted) * 1000000000LL / 48000;
        }
    }
    if (result <= 0) {
        revokePlayback("write_error");
        output.primed = false;
    }
    return result;
}

int standbyOutput(audio_stream* common) {
    std::lock_guard lock(mutex);
    auto* stream = reinterpret_cast<audio_stream_out*>(common);
    auto found = outputs.find(stream);
    if (found == outputs.end()) return -ENODEV;
    const bool alreadyStopped = tetris_dsp_stereo_state().owner == stream;
    if (!revokePlayback("idle")) return -EIO;
    resetHookTargets();
    auto& output = found->second;
    dspRevoking = true;
    const int result = alreadyStopped ? 0 : output.standby(common);
    dspRevoking = false;
    if (result == 0) {
        output.active = output.primed = output.preempted = false;
        output.leaseRetryAfter = {};
    }
    return result;
}

// Offload and MMAP streams may resume without calling write(). Observe their
// real activation instead of treating an idle open descriptor as playback.
int startOutput(const audio_stream_out* stream) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(const_cast<audio_stream_out*>(stream));
    if (found == outputs.end() || !found->second.start) return -ENOSYS;
    if (!revokePlayback("route")) return -EIO;
    const bool wasActive = found->second.active;
    // Treat an attempted activation as live until an explicit stop. Block
    // reentrant mutations while the vendor establishes its playback state.
    found->second.active = true;
    dspRevoking = true;
    const int result = found->second.start(stream);
    dspRevoking = false;
    // ENOSYS rejects unsupported MMAP playback before starting hardware.
    // Keep an existing session, but do not create a phantom active output.
    if (result == -ENOSYS) found->second.active = wasActive;
    return result;
}
int resumeOutput(audio_stream_out* stream) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(stream);
    if (found == outputs.end() || !found->second.resume) return -ENOSYS;
    if (!revokePlayback("route")) return -EIO;
    found->second.active = true;
    dspRevoking = true;
    const int result = found->second.resume(stream);
    dspRevoking = false;
    return result;
}
int stopOutput(const audio_stream_out* stream) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(const_cast<audio_stream_out*>(stream));
    if (found == outputs.end() || !found->second.stop) return -ENOSYS;
    if (!revokePlayback("route")) return -EIO;
    dspRevoking = true;
    const int result = found->second.stop(stream);
    dspRevoking = false;
    if (result == 0) {
        found->second.active = found->second.primed = found->second.preempted = false;
        found->second.leaseRetryAfter = {};
        found->second.dspAttempted = false;
        found->second.hookHandler = nullptr;
        found->second.hookAmbiguous = false;
    }
    return result;
}

// Return this key's exact value, preserving all other parameters for the HAL.
bool splitParameter(const char* parameters, const std::string& key, std::string* value,
                    std::string* remainder, bool* duplicate = nullptr) {
    bool found = false;
    std::string input = parameters ? parameters : "";
    size_t begin = 0;
    while (begin < input.size()) {
        size_t end = input.find(';', begin);
        if (end == std::string::npos) end = input.size();
        std::string item = input.substr(begin, end - begin);
        size_t equal = item.find('=');
        if (item.substr(0, equal) == key) {
            if (found && duplicate) *duplicate = true;
            found = true;
            *value = equal == std::string::npos ? "" : item.substr(equal + 1);
        } else if (!item.empty()) {
            if (!remainder->empty()) *remainder += ';';
            *remainder += item;
        }
        begin = end + 1;
    }
    return found;
}
// Accept only one complete volume message. Additional keys, duplicates and
// malformed values must retain the ordinary route invalidation.
bool isVolumeParameters(const char* parameters, bool dolbyNotification) {
    const std::string_view input = parameters ? parameters : "";
    unsigned seen = 0;
    size_t begin = 0;
    while (begin < input.size()) {
        size_t end = input.find(';', begin);
        if (end == std::string_view::npos) end = input.size();
        const auto item = input.substr(begin, end - begin);
        const size_t equal = item.find('=');
        if (equal == std::string_view::npos || equal + 1 == item.size()) return false;
        const auto key = item.substr(0, equal);
        const unsigned field = dolbyNotification ?
                (key == "volume_change" ? 1 : key == "flags" ? 2 : 0) :
                (key == "volumeIndex" ? 1 : key == "volumeDevice" ? 2 :
                 key == "volumeStreamType" ? 4 : 0);
        if (!field || (seen & field)) return false;
        unsigned value = 0;
        for (char digit : item.substr(equal + 1)) {
            if (digit < '0' || digit > '9' ||
                    value > (std::numeric_limits<int>::max() - unsigned(digit - '0')) / 10) {
                return false;
            }
            value = value * 10 + unsigned(digit - '0');
        }
        if (dolbyNotification) {
            if (field == 2 && value != 8) return false;
        } else if ((field == 2 && value != AUDIO_DEVICE_OUT_SPEAKER) ||
                (field == 4 && value != AUDIO_STREAM_MUSIC)) return false;
        seen |= field;
        if (end < input.size() && end + 1 == input.size()) return false;
        begin = end + 1;
    }
    return seen == (dolbyNotification ? 3u : 7u);
}
bool isSpeakerMusicVolume(const char* parameters) {
    return isVolumeParameters(parameters, false);
}
bool hasSpeakerLease(const audio_stream_out* target = nullptr) {
    if (lease < 0 || active == Mode::Off || active != requested ||
            phoneMode != AUDIO_MODE_NORMAL || inputs || stockWrites || !unmanagedOutputs.empty()) {
        return false;
    }
    const audio_stream_out* owner = nullptr;
    for (const auto& [stream, output] : outputs) {
        if (!output.active) continue;
        if (owner || output.route != AUDIO_DEVICE_OUT_SPEAKER || !output.media ||
                !output.primed || output.preempted || !pcmEligible(stream, output)) return false;
        owner = stream;
    }
    return owner && (!target || owner == target);
}
int setOutputParameters(audio_stream* common, const char* parameters) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(reinterpret_cast<audio_stream_out*>(common));
    if (found == outputs.end()) return -ENODEV;
    const bool volumeRequest = isSpeakerMusicVolume(parameters);
    if (!checkLease()) return -EIO;
    const bool volumeOnly = volumeRequest &&
            hasSpeakerLease(reinterpret_cast<audio_stream_out*>(common));
    // A volume retry is not a new route and must not rearm a preempted lease.
    if (!volumeOnly && !invalidatePlayback("route", false)) return -EIO;
    int result = found->second.setParameters(common, parameters);
    if (result != 0) {
        if (volumeOnly) {
            if (!invalidatePlayback("error")) return -EIO;
            preemptAll("stream_parameters", result);
        } else preemptOutput(found->second, "stream_parameters", result);
    } else if (volumeOnly && !checkLease()) return -EIO;
    if (result == 0) {
        std::string routing, rest;
        bool duplicate = false;
        if (splitParameter(parameters, AUDIO_PARAMETER_STREAM_ROUTING, &routing, &rest, &duplicate)) {
            unsigned route = 0;
            const auto parsed = std::from_chars(routing.data(), routing.data() + routing.size(), route);
            const bool valid = !duplicate && !routing.empty() && parsed.ec == std::errc{} &&
                    parsed.ptr == routing.data() + routing.size();
            confirmOutputRoute(found->second, valid ? static_cast<audio_devices_t>(route) :
                                                       AUDIO_DEVICE_NONE);
        }
    }
    return result;
}
void metadataOutput(audio_stream_out* stream, const source_metadata* metadata) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(stream);
    if (found == outputs.end()) return;
    const bool media = tetris::audio::mediaOnlyMetadata(metadata);
    if (!media) {
        found->second.media = false;
        if (vendorWriteDepth || dspRevoking) preemptOutput(found->second, "metadata");
        if (!revokePlayback("route")) return;
        resetHookTargets();
    }
    if (dspRevoking || vendorWriteDepth) {
        found->second.media = false;
        preemptOutput(found->second, "metadata");
        if (vendorWriteDepth) revokePlayback("route");
        return;
    }
    if (media != found->second.media) found->second.dspAttempted = false;
    found->second.media = media;
    if (found->second.metadata) found->second.metadata(stream, metadata);
}
void metadataOutputV7(audio_stream_out* stream, const source_metadata_v7* metadata) {
    std::lock_guard lock(mutex);
    auto found = outputs.find(stream);
    if (found == outputs.end()) return;
    const bool media = tetris::audio::mediaOnlyMetadata(metadata);
    if (!media) {
        found->second.media = false;
        if (vendorWriteDepth || dspRevoking) preemptOutput(found->second, "metadata");
        if (!revokePlayback("route")) return;
        resetHookTargets();
    }
    if (dspRevoking || vendorWriteDepth) {
        found->second.media = false;
        preemptOutput(found->second, "metadata");
        if (vendorWriteDepth) revokePlayback("route");
        return;
    }
    if (media != found->second.media) found->second.dspAttempted = false;
    found->second.media = media;
    if (found->second.metadataV7) found->second.metadataV7(stream, metadata);
}
int openOutput(audio_hw_device* dev, audio_io_handle_t handle, audio_devices_t route,
               audio_output_flags_t flags, audio_config* config, audio_stream_out** out,
               const char* address) {
    std::lock_guard lock(mutex);
    if (!revokePlayback("route")) return -EIO;
    resetHookTargets();
    int result = original.openOutput(dev, handle, route, flags, config, out, address);
    if (result != 0 || !out || !*out) return result;
    auto* stream = *out;
    Output saved{handle, route, flags, false, false, false, false, nullptr, false,
        stream->write, stream->common.standby, stream->common.set_parameters,
        stream->update_source_metadata, stream->update_source_metadata_v7};
    if (!saved.write || !saved.standby || !saved.setParameters ||
            !stream->common.get_channels || !stream->common.get_format) {
        // Unsupported stream ABI remains completely vendor-owned. Block the
        // experiment while it is open rather than breaking ordinary playback.
        unmanagedOutputs.insert(stream);
        return 0;
    }
    saved.start = stream->start;
    saved.resume = stream->resume;
    saved.stop = stream->stop;
    outputs.emplace(stream, saved);
    if (saved.start) stream->start = startOutput;
    if (saved.resume) stream->resume = resumeOutput;
    if (saved.stop) stream->stop = stopOutput;
    stream->write = writeOutput;
    stream->common.standby = standbyOutput;
    stream->common.set_parameters = setOutputParameters;
    stream->update_source_metadata = metadataOutput;
    stream->update_source_metadata_v7 = metadataOutputV7;
    return 0;
}
void closeOutput(audio_hw_device* dev, audio_stream_out* stream) {
    std::lock_guard lock(mutex);
    if (!revokePlayback("idle")) return;
    resetHookTargets();
    original.closeOutput(dev, stream);
    outputs.erase(stream);
    unmanagedOutputs.erase(stream);
}
int setMode(audio_hw_device* dev, audio_mode_t mode) {
    std::lock_guard lock(mutex);
    if (!invalidatePlayback(mode == AUDIO_MODE_NORMAL ? "idle" : "call")) {
        phoneMode = AUDIO_MODE_IN_CALL;
        return -EIO;
    }
    const int result = original.setMode(dev, mode);
    // A rejected return to NORMAL must not allow media into an existing call.
    phoneMode = result == 0 ? mode : AUDIO_MODE_IN_CALL;
    return result;
}
int openInput(audio_hw_device* dev, audio_io_handle_t handle, audio_devices_t route,
              audio_config* config, audio_stream_in** stream, audio_input_flags_t flags,
              const char* address, audio_source_t source) {
    std::lock_guard lock(mutex);
    if (!invalidatePlayback("route")) return -EIO;
    int result = original.openInput(dev, handle, route, config, stream, flags, address, source);
    if (result == 0) ++inputs;
    return result;
}
void closeInput(audio_hw_device* dev, audio_stream_in* stream) {
    std::lock_guard lock(mutex);
    if (dspRevoking || vendorWriteDepth) return;
    original.closeInput(dev, stream);
    if (inputs) --inputs;
}
int setReceiverGain(const std::string& value) {
    const auto gain = parseReceiverGain(value);
    if (!gain) return -EINVAL;
    if (!consentGranted() && *gain != 0) return -EPERM;
    const auto authorized = parseReceiverGain(
            android::base::GetProperty("persist.sys.tetris.audio_gain", ""));
    if (!authorized || *authorized != *gain) return -EPERM;
    if (!checkLease()) return -EIO;
    if (!refreshCapabilities() || !gainAvailable) return -ENOTSUP;
    if (lease >= 0) {
        __s32 desired = *gain;
        if (ioctl(lease, TETRIS_AUDIO_SET_GAIN, &desired) != 0) {
            const int error = errno;
            if (!revokePlayback("gain_error")) return -EIO;
            resetHookTargets();
            preemptAll("receiver_gain", -error, "gain_error");
            return -error;
        }
    }
    receiverGainDb = *gain;
    reportedGainDb = *gain;
    gainPreferenceLoaded = true;
    return checkLease() ? 0 : -EIO;
}
int setSplitControl(std::string_view key, const std::string& value) {
    const bool gain = key == "tetris_audio_software_gain";
    const bool trim = key == "tetris_audio_sync_trim";
    const auto number = gain ? parseBoundedInteger(value, 0, 6) :
            trim ? parseBoundedInteger(value, -10, 10) : std::optional<int>{};
    const auto output = !gain && !trim ? parseSpeakerOutput(value) : std::optional<SpeakerOutput>{};
    if (!(number || output)) return -EINVAL;
    if (!consentGranted() && (number.value_or(0) != 0 ||
            output.value_or(SpeakerOutput::Both) != SpeakerOutput::Both)) return -EPERM;
    const char* property = gain ? "persist.sys.tetris.audio_software_gain" :
            trim ? "persist.sys.tetris.audio_sync_trim" : "persist.sys.tetris.audio_output";
    if (android::base::GetProperty(property, "") != value) return -EPERM;
    if (!checkLease()) return -EIO;
    if (!refreshCapabilities() || !splitAvailable ||
            !(capabilities.modes & (1u << static_cast<unsigned>(Mode::Stereo)))) return -ENOTSUP;
    if (gain) {
        if (!receiver.setSoftwareGain(*number)) return -EINVAL;
        softwareGainDb = *number;
        softwareGainLoaded = true;
    } else if (trim) {
        // Queued PCM keeps its original clock. Apply this preference at the
        // next receiver session instead of jumping the live deadline.
        syncTrimMs = *number;
        syncTrimLoaded = true;
    } else {
        if (*output != speakerOutput && !invalidatePlayback("idle", false)) return -EIO;
        speakerOutput = *output;
        speakerOutputLoaded = true;
    }
    return 0;
}
int setParameters(audio_hw_device* dev, const char* parameters) {
    std::lock_guard lock(mutex);
    std::string value, remainder;
    bool duplicate = false;
    for (const char* key : {"tetris_audio_software_gain", "tetris_audio_sync_trim",
                           "tetris_audio_output"}) {
        value.clear(); remainder.clear(); duplicate = false;
        if (splitParameter(parameters, key, &value, &remainder, &duplicate)) {
            if (duplicate || !remainder.empty() ||
                    std::string(parameters) != std::string(key) + "=" + value) return -EINVAL;
            return setSplitControl(key, value);
        }
    }
    value.clear(); remainder.clear(); duplicate = false;
    if (splitParameter(parameters, "tetris_audio_gain", &value, &remainder, &duplicate)) {
        if (duplicate || !remainder.empty() ||
                std::string(parameters) != "tetris_audio_gain=" + value) return -EINVAL;
        return setReceiverGain(value);
    }
    remainder.clear();
    bool hasMode = splitParameter(parameters, "tetris_audio_mode", &value, &remainder, &duplicate);
    auto mode = hasMode ? parseMode(value) : std::optional<Mode>{};
    if (hasMode && (!mode || duplicate)) return -EINVAL;
    // AudioSystem parameters are not a privileged control channel by themselves.
    // Only Parts may change this SELinux-protected, persisted authorization.
    if (hasMode && android::base::GetProperty("persist.sys.tetris.audio_mode", "off") != value) {
        return -EPERM;
    }
    if (hasMode && *mode != Mode::Off) {
        if (!consentGranted()) return -EPERM;
        if (!refreshCapabilities() ||
                !(capabilities.modes & (1u << static_cast<unsigned>(*mode))) ||
                ((*mode == Mode::Stereo || speakerOutput != SpeakerOutput::Both) &&
                 (!splitAvailable || !(capabilities.modes &
                         (1u << static_cast<unsigned>(Mode::Stereo)))))) return -ENOTSUP;
    }
    // DSPVolumeSynchronizer sends this Xiaomi notification on every media
    // volume step. The audited MTK HAL rejects both keys without changing a
    // route. Preserve its error result without revoking an existing lease.
    const bool volumeNotification = !hasMode && isVolumeParameters(parameters, true);
    const bool volumeRequest = volumeNotification || (!hasMode && isSpeakerMusicVolume(parameters));
    if (!checkLease()) return -EIO;
    const bool volumeOnly = volumeRequest && hasSpeakerLease();
    // The rejected notification is harmless before the first lease as well.
    // Preserve priming (including a fresh DSP task) and any real preemption.
    const bool preservePlayback = volumeOnly || (volumeNotification && lease < 0);
    // Unrelated device notifications are not evidence that a failed route has
    // recovered. An explicit authorized mode selection may request a retry.
    if (!preservePlayback && !invalidatePlayback("idle", hasMode)) return -EIO;
    int result = preservePlayback ? original.setParameters(dev, parameters) :
            remainder.empty() ? 0 : original.setParameters(dev, remainder.c_str());
    if (result == 0 && hasMode) {
        requested = *mode;
        preferenceLoaded = true;
    }
    const bool unsupportedNotification = volumeNotification && result == -EINVAL;
    if (result != 0 && !unsupportedNotification) {
        if (preservePlayback && !invalidatePlayback("error")) return -EIO;
        preemptAll("device_parameters", result);
    } else if (preservePlayback && !checkLease()) return -EIO;
    return result;
}
char* getParameters(const audio_hw_device* dev, const char* keys) {
    std::lock_guard lock(mutex);
    std::string value, remainder;
    if (!splitParameter(keys, "tetris_audio", &value, &remainder)) {
        return original.getParameters(dev, keys);
    }
    restorePreference();
    checkLease();
    bool available = refreshCapabilities();
    const auto dsp = tetris_dsp_stereo_state();
    bool stereo = available && splitAvailable &&
            (capabilities.modes & (1u << static_cast<unsigned>(Mode::Stereo)));
    std::string response = "tetris_audio_version=1;tetris_audio_available=";
    response += available ? "1" : "0";
    response += ";tetris_audio_diagnostic_version=11";
    response += ";tetris_audio_stereo_available=";
    response += stereo ? "1" : "0";
    const auto receiverState = receiver.state();
    response += ";tetris_audio_split_open=" + std::to_string(receiverState.open);
    response += ";tetris_audio_split_running=" + std::to_string(receiverState.running);
    response += ";tetris_audio_split_error=" + std::to_string(receiverState.error);
    response += ";tetris_audio_split_queued=" + std::to_string(receiverState.queuedFrames);
    response += ";tetris_audio_split_written=" + std::to_string(receiverState.writtenFrames);
    response += ";tetris_audio_split_played=" + std::to_string(receiverState.playedFrames);
    response += ";tetris_audio_split_underruns=" + std::to_string(receiverState.underruns);
    response += ";tetris_audio_split_clock_error_ns=" + std::to_string(receiverState.clockErrorNs);
    response += ";tetris_audio_split_delay_ns=" + std::to_string(receiverDelayNs);
    response += ";tetris_audio_gain_available=";
    response += gainAvailable ? "1" : "0";
    response += ";tetris_audio_gain_db=" + std::to_string(reportedGainDb);
    response += ";tetris_audio_software_gain_available=" + std::to_string(stereo);
    response += ";tetris_audio_software_gain_db=" + std::to_string(softwareGainDb);
    response += ";tetris_audio_sync_trim_available=" + std::to_string(stereo);
    response += ";tetris_audio_sync_trim_ms=" + std::to_string(syncTrimMs);
    response += ";tetris_audio_sync_trim_active_ms=" + std::to_string(activeSyncTrimMs);
    response += ";tetris_audio_output_available=" + std::to_string(stereo);
    response += ";tetris_audio_output=";
    response += speakerOutputName(speakerOutput);
    response += ";tetris_audio_output_active=";
    response += speakerOutputName(activeSpeakerOutput);
    response += ";tetris_audio_requested=";
    response += modeName(requested);
    response += ";tetris_audio_active=";
    response += modeName(leaseMode == Mode::Stereo && !receiverState.running ? Mode::Off : active);
    response += ";tetris_audio_reason=";
    response += leaseMode == Mode::Stereo && !receiverState.running ? "sync_waiting" : reason;
    response += ";tetris_audio_playback_gate=";
    response += playbackGate;
    response += ";tetris_audio_last_preempt=";
    response += lastPreemption;
    response += ";tetris_audio_last_preempt_error=" + std::to_string(lastPreemptionError);
    response += ";tetris_audio_preemptions=" + std::to_string(preemptionCount);
    response += ";tetris_audio_dsp_init_compatible=" + std::to_string(dsp.compatible);
    response += ";tetris_audio_dsp_init_ready=" + std::to_string(dsp.ready);
    response += ";tetris_audio_dsp_init_dirty=" + std::to_string(dsp.dirty);
    response += ";tetris_audio_dsp_init_fault=" + std::to_string(dsp.fault);
    response += ";tetris_audio_dsp_init_attempts=" + std::to_string(dsp.initAttempts);
    response += ";tetris_audio_dsp_init_accepted=" + std::to_string(dsp.initAccepted);
    response += ";tetris_audio_dsp_teardowns=" + std::to_string(dsp.teardowns);
    response += ";tetris_audio_dsp_eligible_writes=" + std::to_string(dsp.eligibleWrites);
    response += ";tetris_audio_dsp_observed_opens=" + std::to_string(dsp.observedOpens);
    response += ";tetris_audio_dsp_eligible_opens=" + std::to_string(dsp.eligibleOpens);
    response += ";tetris_audio_dsp_last_eligible_open_result=" +
            std::to_string(dsp.lastEligibleOpenResult);
    response += ";tetris_audio_dsp_fresh_creates=" + std::to_string(dsp.freshCreates);
    response += ";tetris_audio_dsp_init_packets=" + std::to_string(dsp.initPackets);
    response += ";tetris_audio_dsp_last_gate=";
    response += dsp.lastGate;
    response += ";tetris_audio_dsp_packet_platform=" + std::to_string(dsp.packetPlatform);
    response += ";tetris_audio_dsp_packet_scenario=" + std::to_string(dsp.packetScenario);
    response += ";tetris_audio_dsp_packet_direction=" + std::to_string(dsp.packetDirection);
    response += ";tetris_audio_dsp_handler_count=" + std::to_string(dsp.handlerCount);
    const auto stats = tetris_audio_hook_stats();
    response += ";tetris_audio_hook_sync=" + std::to_string(stats.synchronous);
    response += ";tetris_audio_hook_worker=" + std::to_string(stats.worker);
    response += ";tetris_audio_hook_original=" + std::to_string(stats.original);
    response += ";tetris_audio_backend_normal=" + std::to_string(stats.normal);
    response += ";tetris_audio_backend_fast=" + std::to_string(stats.fast);
    response += ";tetris_audio_backend_dsp=" + std::to_string(stats.dsp);
    response += ";tetris_audio_backend_speaker_protection=" + std::to_string(stats.speakerProtection);
    response += ";tetris_audio_backend_unknown=" + std::to_string(stats.unknown);
    response += ";tetris_audio_input_buffers=" + std::to_string(inputCounts.buffers);
    response += ";tetris_audio_input_frames=" + std::to_string(inputCounts.frames);
    response += ";tetris_audio_input_different=" + std::to_string(inputCounts.different);
    response += ";tetris_audio_input_left_nonzero=" + std::to_string(inputCounts.leftNonzero);
    response += ";tetris_audio_input_right_nonzero=" + std::to_string(inputCounts.rightNonzero);
    const auto bessound = tetris_bessound_probe_stats();
    response += ";tetris_audio_bessound_compatible=";
    response += bessound.compatible ? "1" : "0";
    response += ";tetris_audio_bessound_calls=" + std::to_string(bessound.scopedCalls);
    response += ";tetris_audio_bessound_mono=" + std::to_string(bessound.monoCalls);
    response += ";tetris_audio_bessound_stereo=" + std::to_string(bessound.stereoCalls);
    response += ";tetris_audio_bessound_unknown=" + std::to_string(bessound.unknownCalls);
    response += ";tetris_audio_bessound_input_frames=" + std::to_string(bessound.inputFrames);
    response += ";tetris_audio_bessound_input_different=" + std::to_string(bessound.inputDifferent);
    response += ";tetris_audio_bessound_output_frames=" + std::to_string(bessound.outputFrames);
    response += ";tetris_audio_bessound_output_different=" + std::to_string(bessound.outputDifferent);
    response += ";tetris_audio_bessound_total=" + std::to_string(bessound.totalCalls);
    response += ";tetris_audio_bessound_unscoped=" + std::to_string(bessound.unscopedCalls);
    response += ";tetris_audio_bessound_original_resolved=";
    response += bessound.originalResolved ? "1" : "0";
    response += ";tetris_audio_bessound_wrapper_match=";
    response += bessound.wrapperMatched ? "1" : "0";
    response += ";tetris_audio_bessound_engine_match=";
    response += bessound.engineMatched ? "1" : "0";
    if (!remainder.empty() && !dsp.dirty) {
        char* vendor = original.getParameters(dev, remainder.c_str());
        if (vendor && *vendor) response += ";" + std::string(vendor);
        free(vendor);
    }
    return strdup(response.c_str());
}
int createPatch(audio_hw_device* dev, unsigned numSources, const audio_port_config* sources,
                unsigned numSinks, const audio_port_config* sinks, audio_patch_handle_t* handle) {
    std::lock_guard lock(mutex);
    if (!checkLease() || !invalidatePlayback("route", false)) return -EIO;
    int result = original.createPatch(dev, numSources, sources, numSinks, sinks, handle);
    if (result != 0) { preemptAll("create_patch", result); return result; }
    std::vector<audio_io_handle_t> routed;
    for (unsigned i = 0; i < numSources; ++i) {
        if (sources[i].type != AUDIO_PORT_TYPE_MIX) continue;
        for (auto& [stream, output] : outputs) {
            (void)stream;
            if (output.handle != sources[i].ext.mix.handle) continue;
            confirmOutputRoute(output, numSinks == 1 && sinks[0].type == AUDIO_PORT_TYPE_DEVICE
                    ? sinks[0].ext.device.type : AUDIO_DEVICE_NONE,
                    handle ? std::optional<audio_patch_handle_t>(*handle) : std::nullopt);
            routed.push_back(output.handle);
        }
    }
    if (handle) {
        // An existing patch handle can be updated to different source streams.
        for (auto& [stream, output] : outputs) {
            (void)stream;
            if (output.routePatch == *handle &&
                    std::find(routed.begin(), routed.end(), output.handle) == routed.end()) {
                confirmOutputRoute(output, AUDIO_DEVICE_NONE);
            }
        }
        patches[*handle] = std::move(routed);
    }
    return result;
}
int releasePatch(audio_hw_device* dev, audio_patch_handle_t handle) {
    std::lock_guard lock(mutex);
    if (!checkLease() || !invalidatePlayback("route", false)) return -EIO;
    const int result = original.releasePatch(dev, handle);
    if (result != 0) { preemptAll("release_patch", result); return result; }
    auto found = patches.find(handle);
    if (found != patches.end()) {
        for (auto id : found->second) for (auto& [stream, output] : outputs) {
            (void)stream;
            if (output.handle == id && output.routePatch == handle) {
                confirmOutputRoute(output, AUDIO_DEVICE_NONE);
            }
        }
        patches.erase(found);
    }
    // Patch removal does not stop a vendor PCM/offload/MMAP stream. Keep its
    // activity until a successful stop/standby or close is actually observed.
    return result;
}
int setPort(audio_hw_device* dev, const audio_port_config* config) {
    std::lock_guard lock(mutex);
    const bool volumeRequest = config && config->config_mask == AUDIO_PORT_CONFIG_GAIN &&
            config->type == AUDIO_PORT_TYPE_DEVICE && config->role == AUDIO_PORT_ROLE_SINK &&
            config->ext.device.type == AUDIO_DEVICE_OUT_SPEAKER &&
            config->ext.device.address[0] == '\0' && config->gain.mode == AUDIO_GAIN_MODE_JOINT;
    if (!checkLease()) return -EIO;
    const bool volumeOnly = volumeRequest && hasSpeakerLease();
    if (!volumeOnly && !invalidatePlayback("route", false)) return -EIO;
    const int result = original.setPort(dev, config);
    if (result != 0) {
        if (volumeOnly && !invalidatePlayback("error")) return -EIO;
        preemptAll("port_config", result);
    } else if (volumeOnly && !checkLease()) return -EIO;
    return result;
}
int closeDevice(hw_device_t* dev) {
    std::lock_guard lock(mutex);
    if (!invalidatePlayback("idle")) return -EIO;
    requested = Mode::Off;
    int result = original.close(dev);
    outputs.clear();
    unmanagedOutputs.clear();
    patches.clear();
    inputs = 0;
    device = nullptr;
    return result;
}
int openDevice(const hw_module_t*, const char* name, hw_device_t** out) {
    std::lock_guard lock(mutex);
    if (!out) return -EINVAL;
    if (device) return -EBUSY;
    if (!vendorLibrary) {
        vendorLibrary = dlopen(kVendorHal, RTLD_NOW | RTLD_LOCAL);
        if (!vendorLibrary) { ALOGE("Cannot load vendor HAL: %s", dlerror()); return -ENODEV; }
    }
    const bool hookReady = tetris_audio_hook_initialize(vendorLibrary);
    if (!tetris_dsp_stereo_initialize(vendorLibrary)) {
        ALOGW("Experimental DSP initialization unavailable; retaining stock DSP configuration");
    }
    if (!hookReady) ALOGW("Experimental downmix hook unavailable; retaining stock audio");
    auto* module = static_cast<hw_module_t*>(dlsym(vendorLibrary, HAL_MODULE_INFO_SYM_AS_STR));
    if (!module || !module->id || strcmp(module->id, AUDIO_HARDWARE_MODULE_ID) != 0 ||
            !module->methods || !module->methods->open) return -ENODEV;
    module->dso = vendorLibrary;
    hw_device_t* raw = nullptr;
    int result = module->methods->open(module, name, &raw);
    if (result != 0) return result;
    if (!raw) return -ENODEV;
    auto* dev = reinterpret_cast<audio_hw_device*>(raw);
    if (!hookReady || !dev || dev->common.version < AUDIO_DEVICE_API_VERSION_3_2 ||
            !dev->common.close || !dev->set_mode || !dev->set_parameters ||
            !dev->get_parameters || !dev->open_output_stream || !dev->close_output_stream ||
            !dev->open_input_stream || !dev->close_input_stream) {
        ALOGW("Unsupported optional proxy ABI; retaining original vendor device");
        *out = raw;
        return 0;
    }
    original = {dev->common.close, dev->set_mode, dev->set_parameters, dev->get_parameters,
        dev->open_output_stream, dev->close_output_stream, dev->open_input_stream,
        dev->close_input_stream, dev->create_audio_patch, dev->release_audio_patch,
        dev->set_audio_port_config};
    dev->common.close = closeDevice;
    dev->set_mode = setMode;
    dev->set_parameters = setParameters;
    dev->get_parameters = getParameters;
    dev->open_output_stream = openOutput;
    dev->close_output_stream = closeOutput;
    dev->open_input_stream = openInput;
    dev->close_input_stream = closeInput;
    if (original.createPatch) dev->create_audio_patch = createPatch;
    if (original.releasePatch) dev->release_audio_patch = releasePatch;
    if (original.setPort) dev->set_audio_port_config = setPort;
    device = dev;
    phoneMode = AUDIO_MODE_NORMAL;
    requested = Mode::Off;
    preferenceLoaded = false;
    receiverGainDb = reportedGainDb = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
    gainPreferenceLoaded = gainAvailable = false;
    softwareGainLoaded = syncTrimLoaded = speakerOutputLoaded = false;
    softwareGainDb = syncTrimMs = activeSyncTrimMs = 0;
    speakerOutput = activeSpeakerOutput = SpeakerOutput::Both;
    receiver.setSoftwareGain(0);
    restorePreference();
    refreshCapabilities();
    *out = raw;
    return 0;
}
hw_module_methods_t methods{.open = openDevice};
}

extern "C" {
__attribute__((visibility("default"))) audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_CURRENT,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Tetris event-driven audio proxy",
        .author = "The LineageOS Project",
        .methods = &methods,
        .dso = nullptr,
        .reserved = {},
    },
};
}
