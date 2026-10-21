// SPDX-License-Identifier: Apache-2.0
#include "StereoReceiver.h"
#include "TetrisAudioAbi.h"

#include <tinyalsa/asoundlib.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace tetris::audio {
namespace {
constexpr uint64_t kRate = 48000;
constexpr size_t kPeriodFrames = 240;
constexpr size_t kPeriodCount = 4;
constexpr size_t kMaxBlockFrames = 8192;
constexpr size_t kMaxQueuedFrames = 16384;
constexpr int64_t kSecondNs = 1000000000;
constexpr int64_t kPeriodNs = kSecondNs * kPeriodFrames / kRate;
// Admission bounds, not latency compensation. Targets come from the main
// output's presentation clock plus an explicitly selected measured correction.
constexpr int64_t kClockAgeLimitNs = 250000000;
constexpr int64_t kClockErrorLimitNs = 2 * kPeriodNs;
constexpr int64_t kUnityGain = 1 << 20;
constexpr int64_t kGainSteps[] = {
        kUnityGain, 1176522, 1320079, 1481153, 1661881, 1864661, 2092184};
constexpr unsigned kGainRampFrames = kRate / 100;
constexpr int64_t kLimiterReleaseStep = (kGainSteps[6] - kUnityGain) / (kRate / 20);

int64_t nowNs() {
    timespec time{};
    return clock_gettime(CLOCK_MONOTONIC, &time) == 0 ?
            int64_t(time.tv_sec) * kSecondNs + time.tv_nsec : 0;
}
int64_t framesNs(uint64_t frames) {
    return int64_t(frames / kRate) * kSecondNs +
            int64_t(frames % kRate) * kSecondNs / kRate;
}
int lastError(int fallback = EIO) { return errno > 0 ? errno : fallback; }
}

bool splitStereo(const void* input, size_t bytes, unsigned sampleBits, StereoBlock* result,
                 SpeakerOutput output) {
    if (!result) return false;
    *result = {};
    if (!input || !bytes || (sampleBits != 16 && sampleBits != 32)) return false;
    if (output != SpeakerOutput::Both && output != SpeakerOutput::Upper &&
            output != SpeakerOutput::Lower) return false;
    const size_t frameBytes = sampleBits / 4;
    if (bytes % frameBytes || bytes / frameBytes > kMaxBlockFrames) return false;
    const size_t frames = bytes / frameBytes;
    result->right.assign(bytes, 0);
    result->left.resize(frames);
    result->frameBytes = frameBytes;
    const auto* source = static_cast<const uint8_t*>(input);
    if (output == SpeakerOutput::Lower) {
        std::memcpy(result->right.data(), input, bytes);
        return true;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        if (sampleBits == 16) {
            int16_t pair[2];
            std::memcpy(pair, source + frame * frameBytes, sizeof(pair));
            // Multiplication is defined for negative samples, unlike a signed shift.
            result->left[frame] = output == SpeakerOutput::Upper ?
                    (int32_t(pair[0]) + pair[1]) * 32768 : int32_t(pair[0]) * 65536;
            if (output == SpeakerOutput::Both) {
                std::memcpy(result->right.data() + frame * frameBytes + sizeof(int16_t),
                            &pair[1], sizeof(int16_t));
            }
        } else {
            int32_t pair[2];
            std::memcpy(pair, source + frame * frameBytes, sizeof(pair));
            result->left[frame] = output == SpeakerOutput::Upper ?
                    int32_t((int64_t(pair[0]) + pair[1]) / 2) : pair[0];
            if (output == SpeakerOutput::Both) {
                std::memcpy(result->right.data() + frame * frameBytes + sizeof(int32_t),
                            &pair[1], sizeof(int32_t));
            }
        }
    }
    return true;
}

std::optional<int64_t> receiverTargetTime(PresentationClock clock, uint64_t firstFrame,
                                        int64_t now, int64_t delayNs) {
    if (delayNs < 0 || delayNs > kMaxReceiverDelayNs ||
            now <= 0 || clock.timeNs <= 0 || clock.timeNs > now ||
            now - clock.timeNs > kClockAgeLimitNs || firstFrame < clock.frames) return {};
    const uint64_t ahead = firstFrame - clock.frames;
    if (ahead > kRate / 4) return {};
    const int64_t duration = framesNs(ahead);
    if (clock.timeNs > std::numeric_limits<int64_t>::max() - duration) return {};
    const int64_t uncorrected = clock.timeNs + duration;
    if (uncorrected > std::numeric_limits<int64_t>::max() - delayNs) return {};
    const int64_t target = uncorrected + delayNs;
    if (target < now - kPeriodNs || target - now > kClockAgeLimitNs) return {};
    return target;
}

struct StereoReceiver::Impl {
    struct Pending {
        std::vector<int32_t> samples;
        uint64_t firstFrame;
        int64_t targetNs;
        size_t offset = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::deque<Pending> queue;
    ReceiverState status;
    pcm* output = nullptr;
    int lease = -1;
    bool bound = false;
    bool prepared = false;
    bool stopping = false;
    bool clockStarted = false;
    uint64_t firstFrame = 0;
    uint64_t nextFrame = 0;
    int64_t firstTargetNs = 0;
    int64_t lastTimestampNs = 0;
    unsigned bufferFrames = 0;
    int64_t targetGain = kUnityGain;
    int64_t gain = kUnityGain;
    int64_t limiterGain = kGainSteps[6];
    unsigned rampFrames = 0;

    void resetProcessing() {
        gain = kUnityGain;
        limiterGain = kGainSteps[6];
        rampFrames = targetGain == gain ? 0 : kGainRampFrames;
    }

    void process(std::vector<int32_t>& samples) {
        for (int32_t& sample : samples) {
            if (rampFrames) {
                gain += (targetGain - gain) / rampFrames;
                --rampFrames;
            }
            if (gain == kUnityGain) {
                limiterGain = kGainSteps[6];
                continue;
            }
            const int64_t magnitude = sample < 0 ? -int64_t(sample) : sample;
            const int64_t limit = sample < 0 ? -int64_t(INT32_MIN) : INT32_MAX;
            const int64_t ceiling = magnitude ?
                    std::min(kGainSteps[6], limit * kUnityGain / magnitude) : kGainSteps[6];
            // Immediate peak protection, then a 50 ms release across the gain
            // range. No lookahead or extra samples: the presentation clock is unchanged.
            limiterGain = std::min(ceiling, limiterGain + kLimiterReleaseStep);
            const int64_t amplified = int64_t(sample) * std::min(gain, limiterGain) / kUnityGain;
            sample = int32_t(std::clamp(amplified, int64_t(INT32_MIN), int64_t(INT32_MAX)));
        }
    }

    void fail(int error) {
        status.failed = true;
        status.error = error > 0 ? error : EIO;
        status.running = false;
        if (error == EPIPE) ++status.underruns;
        queue.clear();
        status.queuedFrames = 0;
        // DROP, never drain: pending music must not leak into a subsequent route.
        if (output) pcm_stop(output);
    }

    bool observeClock() {
        if (!status.running) return true;
        // STATUS refreshes hw_ptr/timestamp together. tinyalsa's htimestamp may
        // read a mapped status page without HWSYNC and initially return zero.
        snd_pcm_status state{};
        errno = 0;
        if (pcm_ioctl(output, SNDRV_PCM_IOCTL_STATUS, &state) != 0) {
            fail(lastError());
            return false;
        }
        if (state.state != SNDRV_PCM_STATE_RUNNING) {
            fail(state.state == SNDRV_PCM_STATE_XRUN ? EPIPE : EIO);
            return false;
        }
        const auto available = state.avail;
        const auto stamp = state.tstamp;
        const int64_t now = nowNs();
        if (!lastTimestampNs && !stamp.tv_sec && !stamp.tv_nsec && now > 0 &&
                now - firstTargetNs <= kPeriodNs) return true;
        if (available > bufferFrames || stamp.tv_sec < 0 || stamp.tv_nsec < 0 ||
                stamp.tv_nsec >= kSecondNs || stamp.tv_sec >
                        (std::numeric_limits<int64_t>::max() - stamp.tv_nsec) / kSecondNs) {
            fail(EPROTO);
            return false;
        }
        const uint64_t queued = bufferFrames - available;
        if (queued > status.writtenFrames) {
            fail(EPROTO);
            return false;
        }
        const uint64_t played = status.writtenFrames - queued;
        const int64_t time = int64_t(stamp.tv_sec) * kSecondNs + stamp.tv_nsec;
        if (time <= 0 || time < lastTimestampNs || played < status.playedFrames ||
                now <= 0 || time > now || now - time > kClockAgeLimitNs) {
            fail(EPROTO);
            return false;
        }
        lastTimestampNs = time;
        status.playedFrames = played;
        status.clockErrorNs = time - (firstTargetNs + framesNs(played));
        if (status.clockErrorNs < -kClockErrorLimitNs ||
                status.clockErrorNs > kClockErrorLimitNs) {
            fail(ERANGE);
            return false;
        }
        return true;
    }

    void run() {
        std::unique_lock lock(mutex);
        while (!stopping && !status.failed) {
            if (!observeClock()) break;
            if (queue.empty()) {
                changed.wait_for(lock, std::chrono::nanoseconds(kPeriodNs));
                continue;
            }
            auto& pending = queue.front();
            const int64_t now = nowNs();
            if (!now) { fail(EIO); break; }
            if (!clockStarted) {
                const int64_t wait = pending.targetNs - now;
                if (wait > 0) {
                    changed.wait_for(lock, std::chrono::nanoseconds(wait));
                    continue;
                }
                if (wait < -kPeriodNs) { fail(ETIME); break; }
                firstFrame = pending.firstFrame;
                firstTargetNs = pending.targetNs;
                clockStarted = true;
            }
            if (!status.running && now - firstTargetNs > kPeriodNs) {
                fail(ETIME);
                break;
            }
            const int64_t expected = firstTargetNs +
                    framesNs(pending.firstFrame - firstFrame);
            if (pending.targetNs < expected - kClockErrorLimitNs ||
                    pending.targetNs > expected + kClockErrorLimitNs) {
                fail(ERANGE);
                break;
            }
            snd_xferi transfer{};
            transfer.buf = pending.samples.data() + pending.offset;
            transfer.frames = std::min(kPeriodFrames, pending.samples.size() - pending.offset);
            errno = 0;
            const int result = pcm_ioctl(output, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer);
            const int error = result < 0 ? lastError() :
                    (transfer.result < 0 ? int(-transfer.result) : 0);
            if (error == EAGAIN || error == EINTR || (!error && !transfer.result)) {
                changed.wait_for(lock, std::chrono::milliseconds(1));
                continue;
            }
            if (error || uint64_t(transfer.result) > transfer.frames) {
                fail(error ? error : EPROTO);
                break;
            }
            pending.offset += size_t(transfer.result);
            status.writtenFrames += uint64_t(transfer.result);
            status.queuedFrames -= uint64_t(transfer.result);
            status.running = true;
            if (pending.offset == pending.samples.size()) queue.pop_front();
        }
    }
};

StereoReceiver::StereoReceiver() : impl_(new Impl) {}
StereoReceiver::~StereoReceiver() { close(); }

bool StereoReceiver::start(int leaseFd) {
    close();
    auto& self = *impl_;
    std::lock_guard lock(self.mutex);
    self.status = {};
    self.stopping = false;
    self.prepared = false;
    self.clockStarted = false;
    self.lastTimestampNs = 0;
    self.nextFrame = 0;
    tetris_audio_split_caps caps{};
    errno = 0;
    if (leaseFd < 0 || ioctl(leaseFd, TETRIS_AUDIO_GET_SPLIT_CAPS, &caps) != 0) {
        self.fail(leaseFd < 0 ? EBADF : lastError());
        return false;
    }
    if (caps.version != TETRIS_AUDIO_SPLIT_VERSION || caps.rate != kRate ||
            caps.channels != 1 || caps.format != TETRIS_AUDIO_SPLIT_FORMAT_S32_LE ||
            caps.reserved[0] || caps.reserved[1]) {
        self.fail(ENOTSUP);
        return false;
    }
    pcm_config config{};
    config.channels = 1;
    config.rate = kRate;
    config.period_size = kPeriodFrames;
    config.period_count = kPeriodCount;
    config.format = PCM_FORMAT_S32_LE;
    config.start_threshold = 1;
    config.stop_threshold = kPeriodFrames * kPeriodCount;
    self.output = pcm_open(caps.card, caps.device,
                           PCM_OUT | PCM_MONOTONIC | PCM_NORESTART, &config);
    if (!self.output || !pcm_is_ready(self.output)) {
        self.fail(lastError(ENODEV));
        if (self.output) pcm_close(self.output);
        self.output = nullptr;
        return false;
    }
    const int fd = pcm_get_poll_fd(self.output);
    const int flags = fd < 0 ? -1 : fcntl(fd, F_GETFL);
    __s32 pcmFd = fd;
    self.bufferFrames = pcm_get_buffer_size(self.output);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            self.bufferFrames != kPeriodFrames * kPeriodCount ||
            ioctl(leaseFd, TETRIS_AUDIO_BIND_SPLIT_PCM, &pcmFd) != 0) {
        self.fail(lastError());
        pcm_close(self.output);
        self.output = nullptr;
        return false;
    }
    self.lease = leaseFd;
    self.bound = true;
    self.status.open = true;
    return true;
}

bool StereoReceiver::prepare() {
    auto& self = *impl_;
    std::lock_guard lock(self.mutex);
    if (!self.status.open || self.status.failed || self.stopping) return false;
    if (self.prepared) return true;
    errno = 0;
    if (pcm_prepare(self.output) != 0) {
        self.fail(lastError());
        return false;
    }
    self.prepared = true;
    self.worker = std::thread([&self] { self.run(); });
    return true;
}

bool StereoReceiver::setSoftwareGain(int db) {
    if (db < 0 || db > 6) return false;
    auto& self = *impl_;
    std::lock_guard lock(self.mutex);
    const int64_t target = kGainSteps[db];
    if (target != self.targetGain) {
        self.targetGain = target;
        self.rampFrames = target == self.gain ? 0 : kGainRampFrames;
    }
    return true;
}

bool StereoReceiver::enqueue(std::vector<int32_t> samples, size_t acceptedFrames,
                             uint64_t firstFrame, int64_t targetNs) {
    auto& self = *impl_;
    std::lock_guard lock(self.mutex);
    if (!self.status.open || self.status.failed || self.stopping) return false;
    if (!self.prepared) {
        self.fail(EBADFD);
        return false;
    }
    const int64_t now = nowNs();
    if (!acceptedFrames || acceptedFrames > samples.size() || samples.size() > kMaxBlockFrames ||
            acceptedFrames > kMaxBlockFrames ||
            acceptedFrames > std::numeric_limits<uint64_t>::max() - firstFrame ||
            self.status.queuedFrames + acceptedFrames > kMaxQueuedFrames ||
            (self.nextFrame && firstFrame != self.nextFrame) || now <= 0 ||
            targetNs < now - kPeriodNs || targetNs - now > kClockAgeLimitNs) {
        self.fail(EINVAL);
        self.changed.notify_all();
        return false;
    }
    // A partially accepted block must not retain its discarded tail's storage:
    // queuedFrames bounds allocated sample memory as well as playback duration.
    if (samples.capacity() > acceptedFrames) {
        std::vector<int32_t>(samples.begin(), samples.begin() + acceptedFrames).swap(samples);
    } else {
        samples.resize(acceptedFrames);
    }
    // Transform exactly the accepted prefix once. Partial ALSA writes and
    // EAGAIN retries consume this buffer without applying gain again.
    self.process(samples);
    self.nextFrame = firstFrame + acceptedFrames;
    self.status.queuedFrames += acceptedFrames;
    self.queue.push_back({std::move(samples), firstFrame, targetNs, 0});
    self.changed.notify_all();
    return true;
}

void StereoReceiver::close() {
    auto& self = *impl_;
    {
        std::lock_guard lock(self.mutex);
        self.stopping = true;
        self.queue.clear();
        self.status.queuedFrames = 0;
        self.changed.notify_all();
    }
    // The worker only performs O_NONBLOCK transfers and interruptible waits.
    // It never waits for audio to drain or automatically restarts after an xrun.
    if (self.worker.joinable()) self.worker.join();
    std::lock_guard lock(self.mutex);
    if (self.output) pcm_stop(self.output);
    if (self.bound) {
        const __u32 off = 0;
        const __s32 unbound = -1;
        ioctl(self.lease, TETRIS_AUDIO_SET_MODE, &off);
        ioctl(self.lease, TETRIS_AUDIO_BIND_SPLIT_PCM, &unbound);
    }
    if (self.output) pcm_close(self.output);
    self.output = nullptr;
    self.lease = -1;
    self.bound = false;
    self.prepared = false;
    self.status.open = self.status.running = false;
    self.resetProcessing();
}

ReceiverState StereoReceiver::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}

} // namespace tetris::audio
