// SPDX-License-Identifier: Apache-2.0
#include "../StereoReceiver.h"
#include "../TetrisAudioAbi.h"
#include <tinyalsa/asoundlib.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace tetris::audio;
namespace {
constexpr int kLease = 177;
constexpr int64_t kSecond = 1000000000;
int64_t nowNs() {
    timespec time{};
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return int64_t(time.tv_sec) * kSecond + time.tv_nsec;
}
struct Model {
    std::mutex mutex;
    bool capsBad = false;
    bool bindBad = false;
    bool ready = true;
    bool eagain = false;
    bool xrun = false;
    bool overreport = false;
    bool routeActive = false;
    bool prepared = false;
    int writeError = 0;
    int prepareError = 0;
    int64_t stampShift = 0;
    int64_t zeroStampNs = 0;
    size_t maxWrite = 71;
    int opened = 0, closed = 0, binds = 0, unbinds = 0, modeOff = 0, stops = 0;
    int prepares = 0;
    int retryWrites = 0, retries = 0;
    int fd = -1;
    int64_t startNs = 0;
    uint64_t written = 0;
    std::vector<int32_t> samples;
} model;
void reset() {
    std::lock_guard lock(model.mutex);
    assert(model.fd == -1);
    model.capsBad = model.bindBad = model.eagain = model.xrun = model.overreport = false;
    model.routeActive = model.prepared = false;
    model.ready = true;
    model.writeError = 0;
    model.prepareError = 0;
    model.prepares = 0;
    model.retryWrites = model.retries = 0;
    model.stampShift = 0;
    model.zeroStampNs = 0;
    model.maxWrite = 71;
    model.opened = model.closed = model.binds = model.unbinds = model.modeOff = model.stops = 0;
    model.startNs = 0;
    model.written = 0;
    model.samples.clear();
}
bool waitUntil(const auto& predicate, int milliseconds = 300) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
uint64_t played() {
    return model.startNs ? uint64_t(std::max<int64_t>(0, nowNs() - model.startNs)) * 48000 / kSecond : 0;
}
}

struct pcm {};
extern "C" {
int __wrap_ioctl(int fd, unsigned long command, ...) {
    assert(fd == kLease);
    va_list arguments;
    va_start(arguments, command);
    void* value = va_arg(arguments, void*);
    va_end(arguments);
    std::lock_guard lock(model.mutex);
    if (command == TETRIS_AUDIO_GET_SPLIT_CAPS) {
        *static_cast<tetris_audio_split_caps*>(value) = {
                TETRIS_AUDIO_SPLIT_VERSION, 0, 7, model.capsBad ? 44100u : 48000u,
                1, TETRIS_AUDIO_SPLIT_FORMAT_S32_LE, {0, 0}};
        return 0;
    }
    if (command == TETRIS_AUDIO_BIND_SPLIT_PCM) {
        const int requested = *static_cast<int*>(value);
        if (requested < 0) {
            assert(!model.routeActive);
            ++model.unbinds;
            return 0;
        }
        assert(requested == model.fd);
        assert(!model.prepared && !model.routeActive);
        assert(fcntl(requested, F_GETFL) & O_NONBLOCK);
        ++model.binds;
        if (model.bindBad) { errno = EBUSY; return -1; }
        return 0;
    }
    assert(command == TETRIS_AUDIO_SET_MODE);
    const auto mode = *static_cast<unsigned*>(value);
    assert(mode == 0 || mode == 2);
    if (mode == 0) {
        model.routeActive = false;
        ++model.modeOff;
    } else {
        assert(model.binds > model.unbinds);
        model.routeActive = true;
    }
    return 0;
}
pcm* pcm_open(unsigned card, unsigned device, unsigned flags, pcm_config* config) {
    std::lock_guard lock(model.mutex);
    assert(card == 0 && device == 7);
    assert(flags == (PCM_OUT | PCM_MONOTONIC | PCM_NORESTART));
    assert(config->rate == 48000 && config->channels == 1 && config->format == PCM_FORMAT_S32_LE);
    assert(config->period_size == 240 && config->period_count == 4 && config->start_threshold == 1);
    ++model.opened;
    model.fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(model.fd >= 0);
    return new pcm;
}
int pcm_close(pcm* output) {
    std::lock_guard lock(model.mutex);
    ++model.closed;
    assert(model.fd >= 0);
    close(model.fd);
    model.fd = -1;
    delete output;
    return 0;
}
int pcm_is_ready(pcm*) { return model.ready; }
int pcm_get_poll_fd(pcm*) { return model.fd; }
unsigned pcm_get_buffer_size(pcm*) { return 960; }
int pcm_prepare(pcm*) {
    std::lock_guard lock(model.mutex);
    ++model.prepares;
    // Playback_9 is a dynamic DPCM frontend: no backend before the lease route.
    if (!model.routeActive || model.prepareError) {
        errno = model.prepareError ? model.prepareError : EINVAL;
        return -1;
    }
    model.prepared = true;
    return 0;
}
int pcm_stop(pcm*) {
    std::lock_guard lock(model.mutex);
    ++model.stops;
    model.prepared = false;
    return 0;
}
int pcm_get_htimestamp(pcm*, unsigned* available, timespec* stamp) {
    std::lock_guard lock(model.mutex);
    const uint64_t consumed = played();
    if (model.xrun || consumed > model.written) {
        model.xrun = true;
        errno = EPIPE;
        return -1;
    }
    *available = 960 - unsigned(model.written - consumed);
    const int64_t time = nowNs() + model.stampShift;
    stamp->tv_sec = time / kSecond;
    stamp->tv_nsec = time % kSecond;
    return 0;
}
int pcm_ioctl(pcm*, int command, ...) {
    va_list arguments;
    va_start(arguments, command);
    void* value = va_arg(arguments, void*);
    va_end(arguments);
    std::lock_guard lock(model.mutex);
    if (static_cast<unsigned>(command) == SNDRV_PCM_IOCTL_STATUS) {
        const uint64_t consumed = played();
        if (consumed > model.written) model.xrun = true;
        auto* state = static_cast<snd_pcm_status*>(value);
        state->state = model.xrun ? SNDRV_PCM_STATE_XRUN : SNDRV_PCM_STATE_RUNNING;
        state->avail = 960 - std::min<uint64_t>(960, model.written -
                std::min(model.written, consumed));
        const int64_t now = nowNs();
        const int64_t time = now + model.stampShift;
        if (now - model.startNs >= model.zeroStampNs) {
            state->tstamp.tv_sec = time / kSecond;
            state->tstamp.tv_nsec = time % kSecond;
        }
        return 0;
    }
    assert(static_cast<unsigned>(command) == SNDRV_PCM_IOCTL_WRITEI_FRAMES);
    assert(model.prepared && model.routeActive);
    auto* transfer = static_cast<snd_xferi*>(value);
    if (model.writeError) { errno = model.writeError; return -1; }
    if (model.eagain) { errno = EAGAIN; return -1; }
    if (model.retryWrites) {
        --model.retryWrites;
        ++model.retries;
        errno = EAGAIN;
        return -1;
    }
    if (model.overreport) { transfer->result = transfer->frames + 1; return 0; }
    const uint64_t consumed = played();
    const size_t queued = size_t(model.written - std::min(consumed, model.written));
    const size_t count = std::min({size_t(transfer->frames), model.maxWrite, 960 - queued});
    if (!count) { errno = EAGAIN; return -1; }
    if (!model.startNs) model.startNs = nowNs();
    transfer->result = count;
    const auto* data = static_cast<int32_t*>(transfer->buf);
    model.samples.insert(model.samples.end(), data, data + count);
    model.written += count;
    return 0;
}
}

bool startReady(StereoReceiver& receiver) {
    if (!receiver.start(kLease)) return false;
    const unsigned stereo = 2;
    if (ioctl(kLease, TETRIS_AUDIO_SET_MODE, &stereo) != 0) return false;
    return receiver.prepare();
}

std::vector<int32_t> finishPlayback(StereoReceiver& receiver, size_t frames) {
    assert(waitUntil([&] {
        const auto state = receiver.state();
        return state.writtenFrames == frames || state.failed;
    }));
    assert(!receiver.state().failed);
    receiver.close();
    std::lock_guard lock(model.mutex);
    assert(model.samples.size() == frames);
    return model.samples;
}

int main() {
    StereoBlock block;
    const int16_t sixteen[] = {INT16_MIN, 123, INT16_MAX, -456, -1, 0, 0, INT16_MIN};
    assert(splitStereo(sixteen, sizeof(sixteen), 16, &block));
    assert((block.left == std::vector<int32_t>{INT32_MIN, 2147418112, -65536, 0}));
    const int16_t expected[] = {0, 123, 0, -456, 0, 0, 0, INT16_MIN};
    assert(block.right.size() == sizeof(expected));
    assert(std::memcmp(block.right.data(), expected, sizeof(expected)) == 0);
    assert(block.frameBytes == 4 && sixteen[0] == INT16_MIN);
    const int32_t thirtytwo[] = {INT32_MIN, INT32_MAX, -1, 12345, 0, -999};
    // Unaligned input must also work.
    std::vector<uint8_t> unaligned(sizeof(thirtytwo) + 1);
    std::memcpy(unaligned.data() + 1, thirtytwo, sizeof(thirtytwo));
    assert(splitStereo(unaligned.data() + 1, sizeof(thirtytwo), 32, &block));
    assert((block.left == std::vector<int32_t>{INT32_MIN, -1, 0}));
    assert(!splitStereo(sixteen, 3, 16, &block) && block.left.empty());
    assert(!splitStereo(sixteen, sizeof(sixteen), 24, &block));
    assert(!splitStereo(nullptr, sizeof(sixteen), 16, &block));

    // A single selected speaker retains both input channels. Averaging must
    // remain defined at the signed PCM limits and preserve the caller's buffer.
    const int16_t upper16[] = {INT16_MIN, INT16_MIN, INT16_MAX, INT16_MAX,
                              INT16_MIN, INT16_MAX, 0, 2};
    assert(splitStereo(upper16, sizeof(upper16), 16, &block, SpeakerOutput::Upper));
    assert((block.left == std::vector<int32_t>{INT32_MIN, 2147418112, -32768, 65536}));
    assert(std::all_of(block.right.begin(), block.right.end(), [](uint8_t b) { return b == 0; }));
    assert(splitStereo(upper16, sizeof(upper16), 16, &block, SpeakerOutput::Lower));
    assert((block.left == std::vector<int32_t>(4, 0)));
    assert(std::memcmp(block.right.data(), upper16, sizeof(upper16)) == 0);
    const int32_t upper32[] = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX,
                              INT32_MIN, INT32_MAX, 0, 2};
    std::vector<uint8_t> upperUnaligned(sizeof(upper32) + 1);
    std::memcpy(upperUnaligned.data() + 1, upper32, sizeof(upper32));
    assert(splitStereo(upperUnaligned.data() + 1, sizeof(upper32), 32, &block,
                       SpeakerOutput::Upper));
    assert((block.left == std::vector<int32_t>{INT32_MIN, INT32_MAX, 0, 1}));
    assert(std::all_of(block.right.begin(), block.right.end(), [](uint8_t b) { return b == 0; }));
    assert(std::memcmp(upperUnaligned.data() + 1, upper32, sizeof(upper32)) == 0);
    assert(splitStereo(upper32, sizeof(upper32), 32, &block, SpeakerOutput::Lower));
    assert((block.left == std::vector<int32_t>(4, 0)));
    assert(std::memcmp(block.right.data(), upper32, sizeof(upper32)) == 0);
    assert(!splitStereo(upper32, sizeof(upper32), 32, &block, SpeakerOutput(99)));

    constexpr int64_t time = 1000000000;
    assert(receiverTargetTime({1000, time}, 1480, time + 1000) == time + 10000000);
    assert(!receiverTargetTime({1001, time}, 1000, time));
    assert(!receiverTargetTime({1000, time + 1}, 1001, time));
    assert(!receiverTargetTime({1000, time}, 20000, time));
    assert(!receiverTargetTime({1000, time}, 2000, time + 251000000));

    // A measured output delay shifts only the final deadline. Keep the vendor
    // anchor and the zero-delay path unchanged, including their admission bounds.
    const PresentationClock anchor{1000, time};
    assert(receiverTargetTime(anchor, 1480, time, 0) ==
            receiverTargetTime(anchor, 1480, time));
    assert(receiverTargetTime(anchor, 1480, time, kDeepBufferReceiverDelayNs) ==
            time + 82000000);
    assert(anchor.frames == 1000 && anchor.timeNs == time);
    assert(!receiverTargetTime(anchor, 1480, time, -1));
    assert(receiverTargetTime(anchor, 1480, time, kMaxReceiverDelayNs) == time + 92000000);
    assert(!receiverTargetTime(anchor, 1480, time, kMaxReceiverDelayNs + 1));
    assert(!receiverTargetTime(anchor, 1480, time, INT64_MAX));
    assert(receiverTargetTime(anchor, 1000 + 8544, time, kDeepBufferReceiverDelayNs) ==
            time + 250000000);
    assert(!receiverTargetTime(anchor, 1000 + 8545, time, kDeepBufferReceiverDelayNs));
    assert(receiverTargetTime(anchor, 1000 + 8064, time, kMaxReceiverDelayNs) ==
            time + 250000000);
    assert(!receiverTargetTime(anchor, 1000 + 8065, time, kMaxReceiverDelayNs));
    assert(receiverTargetTime(anchor, 1000 + 9600, time) == time + 200000000);
    assert(!receiverTargetTime(anchor, 1000 + 9600, time, kDeepBufferReceiverDelayNs));
    assert(!receiverTargetTime(anchor, 1480, time + 251000000, kDeepBufferReceiverDelayNs));
    const int64_t nearLimit = INT64_MAX - 82000000;
    assert(receiverTargetTime({1000, nearLimit}, 1480, nearLimit,
                              kDeepBufferReceiverDelayNs) == INT64_MAX);
    assert(!receiverTargetTime({1000, nearLimit + 1}, 1480, nearLimit + 1,
                               kDeepBufferReceiverDelayNs));
    assert(!receiverTargetTime({1000, INT64_MAX}, 1480, INT64_MAX));

    reset();
    {
        // Default gain is exactly transparent, including negative full scale.
        StereoReceiver receiver;
        assert(startReady(receiver));
        std::vector<int32_t> source(2048);
        const int32_t values[] = {INT32_MIN, INT32_MAX, -1, 0, 1, -123456789};
        for (size_t i = 0; i < source.size(); ++i) source[i] = values[i % 6];
        assert(receiver.enqueue(source, source.size(), 0, nowNs() + 20000000));
        assert(finishPlayback(receiver, source.size()) == source);
    }
    reset();
    {
        StereoReceiver receiver;
        for (int db = 0; db <= 6; ++db) assert(receiver.setSoftwareGain(db));
        assert(!receiver.setSoftwareGain(-1) && !receiver.setSoftwareGain(7));
        assert(startReady(receiver));
        constexpr int32_t sample = 1 << 28;
        const int64_t target = nowNs() + 20000000;
        // Only the accepted prefix may advance the ramp or affect the limiter.
        std::vector<int32_t> prefix(3000, INT32_MAX);
        std::fill_n(prefix.begin(), 240, sample);
        assert(receiver.enqueue(std::move(prefix), 240, 0, target));
        assert(receiver.setSoftwareGain(6)); // Repeating a setting must not restart its ramp.
        assert(receiver.enqueue(std::vector<int32_t>(784, sample), 784, 240, target + 5000000));
        const auto first = finishPlayback(receiver, 1024);
        assert(first.front() > sample && first.front() < sample + sample / 200);
        assert(std::is_sorted(first.begin(), first.end()));
        assert(first[239] < first[479] && first[478] < first[479]);
        assert(first[479] > sample * int64_t(19) / 10 && first[479] < sample * int64_t(2));
        assert(std::all_of(first.begin() + 479, first.end(), [&](int32_t s) { return s == first[479]; }));

        // A fresh stream resets the envelope but retains +6 dB. Partial writes
        // and repeated EAGAIN must reproduce exactly the same accepted PCM.
        reset();
        model.retryWrites = 2;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(1024, sample), 1024, 0, nowNs() + 20000000));
        assert(finishPlayback(receiver, 1024) == first);
        assert(model.retries == 2);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(receiver.setSoftwareGain(6) && startReady(receiver));
        constexpr int32_t sample = 1 << 28;
        std::vector<int32_t> source(2048, sample);
        source[512] = INT32_MAX;
        source[513] = INT32_MIN;
        assert(receiver.enqueue(source, source.size(), 0, nowNs() + 20000000));
        const auto boosted = finishPlayback(receiver, source.size());
        // Peaks stay in range without wrapping; following samples recover
        // gradually instead of returning immediately to the boosted amplitude.
        assert(boosted[512] == INT32_MAX && boosted[513] == INT32_MIN);
        assert(boosted[514] >= sample && boosted[514] < boosted[511]);
        assert(boosted[515] > boosted[514] && boosted.back() < boosted[511]);
        assert(std::is_sorted(boosted.begin() + 514, boosted.end()));

        reset();
        assert(receiver.setSoftwareGain(0) && startReady(receiver));
        assert(receiver.enqueue(source, source.size(), 0, nowNs() + 20000000));
        assert(finishPlayback(receiver, source.size()) == source);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(receiver.setSoftwareGain(6) && startReady(receiver));
        constexpr int32_t sample = 1 << 28;
        const int64_t target = nowNs() + 20000000;
        assert(receiver.enqueue(std::vector<int32_t>(480, sample), 480, 0, target));
        assert(receiver.setSoftwareGain(0));
        assert(receiver.enqueue(std::vector<int32_t>(1024, sample), 1024, 480, target + 10000000));
        const auto ramp = finishPlayback(receiver, 1504);
        assert(ramp[480] < ramp[479] && ramp[480] > sample);
        assert(std::is_sorted(ramp.begin() + 480, ramp.begin() + 960, std::greater<int32_t>()));
        assert(std::all_of(ramp.begin() + 959, ramp.end(), [&](int32_t s) { return s == sample; }));
    }

    reset();
    {
        StereoReceiver receiver;
        assert(receiver.start(kLease));
        assert(receiver.state().open && !receiver.state().running);
        assert(model.prepares == 0 && model.binds == 1 && !model.routeActive);
        assert(!receiver.prepare());
        assert(receiver.state().error == EINVAL && model.prepares == 1);
        receiver.close();
        assert(model.modeOff == 1 && model.unbinds == 1 && model.closed == 1);
        assert(receiver.state().error == EINVAL);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(receiver.start(kLease));
        assert(!receiver.enqueue(std::vector<int32_t>(240, 1), 240, 0, nowNs() + 2000000));
        assert(receiver.state().error == EBADFD && model.samples.empty());
        receiver.close();
        assert(model.prepares == 0 && model.unbinds == 1 && model.closed == 1);
    }
    reset();
    {
        // Preparation failure after enabling the backend still revokes the
        // route and binding before closing the PCM, retaining the cause.
        StereoReceiver receiver;
        model.prepareError = EIO;
        assert(!startReady(receiver));
        assert(model.routeActive && receiver.state().error == EIO);
        receiver.close();
        assert(!model.routeActive && model.unbinds == 1 && model.closed == 1);
        assert(receiver.state().error == EIO);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(startReady(receiver));
        const int64_t target = nowNs() + 20000000;
        std::vector<int32_t> left(3000);
        for (size_t i = 0; i < left.size(); ++i) left[i] = int32_t(i + 1);
        // The main branch accepted only this prefix; trailing samples must never play.
        assert(receiver.enqueue(std::move(left), 2048, 10000, target));
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        { std::lock_guard lock(model.mutex); assert(model.samples.empty()); }
        assert(waitUntil([&] { return receiver.state().writtenFrames == 2048 || receiver.state().failed; }));
        assert(!receiver.state().failed);
        receiver.close();
        std::lock_guard lock(model.mutex);
        assert(model.samples.size() == 2048);
        for (size_t i = 0; i < model.samples.size(); ++i) assert(model.samples[i] == int32_t(i + 1));
        assert(model.binds == 1 && model.unbinds == 1 && model.modeOff == 1 && model.closed == 1);
    }
    reset();
    {
        // Both accepted blocks use the same corrected frame timeline. The
        // receiver must neither write before that deadline nor count the
        // acoustic correction as a hardware clock error after it starts.
        StereoReceiver receiver;
        assert(startReady(receiver));
        const int64_t base = nowNs();
        const auto first = receiverTargetTime({1000, base}, 1480, base,
                                             kDeepBufferReceiverDelayNs);
        const auto second = receiverTargetTime({1000, base}, 3528, base,
                                              kDeepBufferReceiverDelayNs);
        assert(first && second && *second - *first == 42666666);
        assert(receiver.enqueue(std::vector<int32_t>(2048, 3), 2048, 1480, *first));
        assert(receiver.enqueue(std::vector<int32_t>(2048, 4), 2048, 3528, *second));
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        { std::lock_guard lock(model.mutex); assert(model.samples.empty()); }
        assert(waitUntil([&] { return receiver.state().writtenFrames == 4096 || receiver.state().failed; }));
        const auto state = receiver.state();
        assert(!state.failed && state.running && state.underruns == 0);
        assert(state.clockErrorNs > -5000000 && state.clockErrorNs < 5000000);
        receiver.close();
        std::lock_guard lock(model.mutex);
        assert(model.startNs >= *first && model.samples.size() == 4096);
        assert(std::all_of(model.samples.begin(), model.samples.begin() + 2048,
                           [](int32_t s) { return s == 3; }));
        assert(std::all_of(model.samples.begin() + 2048, model.samples.end(),
                           [](int32_t s) { return s == 4; }));
    }
    reset();
    {
        StereoReceiver receiver;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(240, 7), 240, 1, nowNs() + 200000000));
        const auto began = std::chrono::steady_clock::now();
        receiver.close();
        assert(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(50));
        assert(model.samples.empty());
    }
    for (const int failure : {EPIPE, EIO, EAGAIN, EPROTO}) {
        reset();
        StereoReceiver receiver;
        if (failure == EAGAIN) model.eagain = true;
        else if (failure == EPROTO) model.overreport = true;
        else model.writeError = failure;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(240, 1), 240, 1, nowNs() + 2000000));
        assert(waitUntil([&] { return receiver.state().failed; }));
        assert(receiver.state().error == (failure == EAGAIN ? ETIME : failure));
        receiver.close();
    }
    reset();
    {
        // A newly started ALSA stream may not have a hardware timestamp yet.
        // Keep filling through the first partial writes, without extending the
        // grace once playback has obtained a clock.
        StereoReceiver receiver;
        model.zeroStampNs = 2000000;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(2048, 9), 2048, 0, nowNs() + 2000000));
        assert(waitUntil([&] { return receiver.state().writtenFrames == 2048 || receiver.state().failed; }));
        assert(!receiver.state().failed);
        receiver.close();
    }
    for (const bool xrun : {false, true}) {
        reset();
        StereoReceiver receiver;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(8192, 9), 8192, 0, nowNs() + 2000000));
        assert(waitUntil([&] { return receiver.state().playedFrames >= 300 || receiver.state().failed; }));
        assert(!receiver.state().failed);
        {
            std::lock_guard lock(model.mutex);
            if (xrun) model.xrun = true;
            else model.stampShift = -20000000;
        }
        assert(waitUntil([&] { return receiver.state().failed; }));
        assert(receiver.state().error == (xrun ? EPIPE : EPROTO));
        assert(receiver.state().queuedFrames == 0 && !receiver.state().running);
        receiver.close();
    }
    reset();
    {
        StereoReceiver receiver;
        model.zeroStampNs = 100000000;
        assert(startReady(receiver));
        assert(receiver.enqueue(std::vector<int32_t>(8192, 9), 8192, 0, nowNs() + 2000000));
        assert(waitUntil([&] { return receiver.state().failed; }));
        assert(receiver.state().error == EPROTO);
        receiver.close();
    }
    reset();
    {
        // A valid but misaligned hardware clock must not silently drift away
        // from the vendor stream's presentation deadline.
        StereoReceiver receiver;
        model.stampShift = -20000000;
        assert(startReady(receiver));
        const int64_t base = nowNs();
        const auto target = receiverTargetTime({0, base}, 96, base,
                                              kDeepBufferReceiverDelayNs);
        assert(target && receiver.enqueue(std::vector<int32_t>(8192, 9), 8192, 96, *target));
        assert(waitUntil([&] { return receiver.state().failed; }));
        assert(receiver.state().error == ERANGE);
        receiver.close();
    }
    reset();
    {
        StereoReceiver receiver;
        assert(startReady(receiver));
        const int64_t target = nowNs() + 20000000;
        assert(receiver.enqueue(std::vector<int32_t>(240, 1), 240, 100, target));
        assert(receiver.enqueue(std::vector<int32_t>(240, 2), 240, 340, target + 100000000));
        assert(waitUntil([&] { return receiver.state().failed; }));
        assert(receiver.state().error == ERANGE);
        receiver.close();
        assert(model.samples.size() == 240);
        assert(std::all_of(model.samples.begin(), model.samples.end(), [](int32_t s) { return s == 1; }));
    }
    reset();
    {
        StereoReceiver receiver;
        model.capsBad = true;
        assert(!receiver.start(kLease) && model.opened == 0);
        model.capsBad = false;
        model.bindBad = true;
        assert(!receiver.start(kLease) && model.closed == 1 && model.unbinds == 0);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(startReady(receiver));
        const int64_t base = nowNs();
        const auto target = receiverTargetTime({0, base}, 6144, base,
                                              kDeepBufferReceiverDelayNs);
        assert(target);
        assert(receiver.enqueue(std::vector<int32_t>(8192, 1), 8192, 6144, *target));
        assert(receiver.enqueue(std::vector<int32_t>(8192, 1), 8192, 14336, *target));
        assert(!receiver.enqueue(std::vector<int32_t>(1, 1), 1, 22528, *target));
        assert(receiver.state().failed && receiver.state().queuedFrames == 0);
    }
    reset();
    {
        StereoReceiver receiver;
        assert(startReady(receiver));
        const int64_t target = nowNs() + 200000000;
        assert(receiver.enqueue(std::vector<int32_t>(240, 1), 240, 100, target));
        assert(!receiver.enqueue(std::vector<int32_t>(240, 1), 240, 341, target));
        assert(receiver.state().failed);
    }
    std::puts("StereoReceiver tests passed");
}
