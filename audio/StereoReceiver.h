// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace tetris::audio {

// Input is the already mixed and volume-scaled Android PCM. Splitting does not
// change its gain; the optional receiver boost is applied only on enqueue.
struct StereoBlock {
    std::vector<uint8_t> right;
    std::vector<int32_t> left;
    size_t frameBytes = 0;
};
enum class SpeakerOutput { Both, Upper, Lower };
bool splitStereo(const void* input, size_t bytes, unsigned sampleBits, StereoBlock* result,
                 SpeakerOutput output = SpeakerOutput::Both);

struct PresentationClock {
    uint64_t frames = 0;
    int64_t timeNs = 0;
};
// Measured acoustic correction for the 48 kHz PCM32 deep-buffer speaker path.
// Other output paths must retain zero delay until measured separately.
inline constexpr int64_t kDeepBufferReceiverDelayNs = 72000000;
inline constexpr int64_t kMaxReceiverDelayNs = kDeepBufferReceiverDelayNs + 10000000;
// Frames are in the vendor stream's cumulative 48 kHz frame domain. A clock
// which is stale, ahead of the local clock or inconsistent is not usable. The
// optional correction shifts only the receiver deadline, not the vendor clock;
// the final deadline must still fit the same bounded queue admission window.
std::optional<int64_t> receiverTargetTime(PresentationClock clock, uint64_t firstFrame,
                                        int64_t nowNs, int64_t delayNs = 0);

struct ReceiverState {
    bool open = false;
    bool running = false;
    bool failed = false;
    int error = 0;
    uint64_t queuedFrames = 0;
    uint64_t writtenFrames = 0;
    uint64_t playedFrames = 0;
    uint64_t underruns = 0;
    int64_t clockErrorNs = 0;
};

class StereoReceiver {
  public:
    StereoReceiver();
    ~StereoReceiver();
    StereoReceiver(const StereoReceiver&) = delete;
    StereoReceiver& operator=(const StereoReceiver&) = delete;

    // Opens/configures the dedicated PCM and binds its descriptor while in SETUP.
    // The caller must enable the lease's stereo route before prepare().
    bool start(int leaseFd);
    // DPCM requires a connected backend before ALSA PREPARE. Starts the worker
    // only after preparation succeeds; does not enable a route or change gain.
    bool prepare();
    // Receiver PCM only, 0..6 dB. Changes ramp over 10 ms of accepted samples;
    // peak limiting adds no buffering. Zero gain is bit-exact after the ramp.
    // The requested setting survives close/start; each stream ramps from unity.
    bool setSoftwareGain(int db);
    // Only enqueue frames accepted by the main output. targetNs is the predicted
    // presentation time of firstFrame, never a fixed delay from write().
    bool enqueue(std::vector<int32_t> samples, size_t acceptedFrames,
                 uint64_t firstFrame, int64_t targetNs);
    // Cancels queued data and joins the nonblocking worker before unbinding.
    void close();
    ReceiverState state() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tetris::audio
