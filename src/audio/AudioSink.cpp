// AudioSink.cpp
#define MINIAUDIO_IMPLEMENTATION
#include "audio/AudioSink.hpp"
#include "util/Log.hpp"
#include "miniaudio.h"

namespace temporal_forge {

namespace {
// deviceDeleter: custom deleter bridging ma_device destruction into unique_ptr.
//                Called automatically when AudioSink::device_ resets. Uninits the
//                device (releases miniaudio resources) then frees the allocation.
void deviceDeleter(ma_device* d) {
    if (d) {
        ma_device_uninit(d);
        delete d;
    }
}
} // namespace

// AudioRing capacity: ~1 second of stereo float @ 48k = 192000 samples, rounded
// up to 256k (power of two).
AudioSink::AudioSink()
    : ring_(std::make_unique<AudioRing>(262144)) {}

// ~AudioSink: stop the device before members are destroyed (header contract).
AudioSink::~AudioSink() { stop(); }

// start: create and start the miniaudio playback device, then clear the ring
// and audio clock. Upstream AudioDecoder/PlaybackEngine push decoded samples;
// downstream the realtime callback drains them and defines the primary A/V
// clock. A failure leaves no live device and returns false to the caller.
bool AudioSink::start(int channels, int sampleRate) {
    stop();
    channels_ = channels;
    sampleRate_ = sampleRate;
    ring_->clear();
    consumedTotal_.store(0, std::memory_order_relaxed);

    auto* dev = new ma_device{};
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(channels);
    cfg.sampleRate = static_cast<ma_uint32>(sampleRate);
    cfg.dataCallback = &AudioSink::dataCallback;
    cfg.pUserData = this;
    cfg.periodSizeInFrames = static_cast<ma_uint32>(sampleRate / 100); // ~10ms

    if (ma_device_init(nullptr, &cfg, dev) != MA_SUCCESS) {
        logError("AudioSink: ma_device_init failed");
        delete dev;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        logError("AudioSink: ma_device_start failed");
        ma_device_uninit(dev);
        delete dev;
        return false;
    }
    device_ = {dev, deviceDeleter};
    logInfo("AudioSink: started {}ch {}Hz", channels, sampleRate);
    return true;
}

// stop: destroy the device through deviceDeleter. This is safe from repeated
// playlist changes and destructor cleanup; the ring remains allocated so a
// subsequent start can reuse it.
void AudioSink::stop() {
    if (device_) device_.reset(); // calls deleter -> uninit + delete
}

// push: enqueue decoded interleaved float samples without blocking the decode
// thread. If the realtime device has not drained enough data, the remainder is
// dropped intentionally; audio owns the clock and blocking here would stall
// video decode and temporal frame production.
void AudioSink::push(const float* samples, size_t count) {
    if (!ring_) return;
    size_t off = 0;
    while (off < count) {
        size_t w = ring_->write(samples + off, count - off);
        if (w == 0) {
            // Ring full; the device will drain it. Drop remainder to avoid
            // blocking the decode thread (audio device owns the clock).
            break;
        }
        off += w;
    }
}

// setStartPts: align the audio clock with the first decoded audio PTS and reset
// consumed-sample accounting. PlaybackEngine calls this after a seek/new file.
void AudioSink::setStartPts(int64_t ptsUs) {
    std::lock_guard lock(startMutex_);
    startPtsUs_.store(ptsUs, std::memory_order_relaxed);
    consumedTotal_.store(0, std::memory_order_relaxed);
}

// clockUs: derive presentation time from the immutable start PTS plus samples
// actually consumed by the device. PlaybackEngine compares video PTS against
// this value for pacing; it is deliberately not based on queue depth.
int64_t AudioSink::clockUs() const {
    const int64_t start = startPtsUs_.load(std::memory_order_acquire);
    if (start < 0) return -1;
    const uint64_t consumed = consumedTotal_.load(std::memory_order_acquire);
    const double seconds = static_cast<double>(consumed) /
                          (static_cast<double>(channels_) * sampleRate_);
    return start + static_cast<int64_t>(seconds * 1e6);
}

// setVolume: clamp a UI value before publishing it to the realtime callback.
// The atomic avoids taking a mutex in the audio thread.
void AudioSink::setVolume(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    volume_.store(v, std::memory_order_relaxed);
}
// setMuted: publish the mute switch read by dataCallback without interrupting
// the audio device.
void AudioSink::setMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }

// dataCallback: realtime miniaudio boundary. It must remain allocation-free
// and non-blocking: read queued samples, advance the audio clock, and apply the
// current volume/mute atomics before handing the buffer to the device.
void AudioSink::dataCallback(ma_device* device, void* output, const void* /*input*/,
                             uint32_t frameCount) {
    auto* self = static_cast<AudioSink*>(device->pUserData);
    if (!self || !output) return;

    const size_t sampleCount = static_cast<size_t>(frameCount) * self->channels_;
    auto* out = static_cast<float*>(output);

    const size_t got = self->ring_->read(out, sampleCount);
    self->consumedTotal_.fetch_add(got, std::memory_order_relaxed);

    // Apply volume / mute on the realtime thread (cheap).
    const float vol = self->muted_.load(std::memory_order_relaxed)
                      ? 0.0f
                      : self->volume_.load(std::memory_order_relaxed);
    if (vol == 0.0f) {
        std::fill_n(out, sampleCount, 0.0f);
    } else if (vol != 1.0f) {
        for (size_t i = 0; i < sampleCount; ++i) out[i] *= vol;
    }
}

} // namespace temporal_forge
