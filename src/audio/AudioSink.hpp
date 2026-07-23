// AudioSink.hpp — miniaudio-backed audio output. The audio device callback
// is the master clock (spec 01 "Clocking: Primary clock: audio clock").
//
//   audioClockUs = startPtsUs + (samplesConsumed / sampleRate) * 1e6
//
// Never slow audio to wait for the video path (spec 01). If video falls
// behind, the video frame pacing handles lateness; audio runs free.
#pragma once
#include "audio/RingBuffer.hpp"
#include <atomic>
#include <memory>
#include <mutex>

// miniaudio is a single header; we include it once here with the impl in
// the corresponding .cpp.
struct ma_device;

namespace temporal_forge {

class AudioSink {
public:
    AudioSink();
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    // start: open and start the miniaudio playback device.
    //
    // Called by: PlaybackEngine::openUrl (first device open) and ::play (resume
    //            after pause), with channels/sampleRate from AudioDecoder.
    // Calls:     ma_device_init, ma_device_start; clears the ring + consumedTotal.
    // Returns:   false (without leaving a half-open device) on init/start failure.
    // Notes:     Configures ~10ms periods (sampleRate/100 frames). The data
    //            callback is AudioSink::dataCallback.
    bool start(int channels, int sampleRate);

    // stop: stop and uninitialize the device (idempotent; safe when not running).
    //       Called by: PlaybackEngine::pause, ::close, ::stopThreads, and start (reconfig).
    void stop();

    // push: feed interleaved float samples into the ring buffer.
    //
    // Called by: PlaybackEngine::audioDecodeLoop (audio decode thread, producer).
    // Calls:     AudioRing::write (drops remainder if the ring is full rather than
    //            blocking — the audio device owns the clock, spec 01).
    void push(const float* samples, size_t count);

    // --- master clock (read on the video/pacing thread) ---

    // setStartPts: record the PTS (us) of the first sample about to be pushed
    //              and zero the consumed counter. Rebases the audio clock.
    //
    // Called by: PlaybackEngine::play (rebase on resume), ::seekUs (rebase at
    //            seek target), ::audioDecodeLoop (set on first chunk).
    // Notes:     Guarded by startMutex_ so the rebase is atomic with the
    //            consumed-counter reset; clockUs() reads both atomically.
    void setStartPts(int64_t ptsUs);

    // clockUs: current audio master clock in microseconds, or -1 before the
    //          first sample is pushed.
    //          Called by: PlaybackEngine::positionUs and the video pacing logic
    //          (video aligns to this clock; audio never waits for video).
    [[nodiscard]] int64_t clockUs() const;

    // samplesConsumed: total samples pulled by the device so far.
    //                  Used by clockUs()'s computation.
    [[nodiscard]] uint64_t samplesConsumed() const;

    // setVolume / setMuted: thread-safe volume + mute (0..1). Applied in the
    //                       realtime dataCallback. Called by PlaybackEngine::setVolume/
    //                       setMuted (from QML volume property).
    void setVolume(float v); // 0..1
    [[nodiscard]] float volume() const { return volume_.load(); }
    void setMuted(bool m);
    [[nodiscard]] bool muted() const { return muted_.load(); }

    // clear: empty the ring buffer. Called by PlaybackEngine::close.
    void clear() { ring_->clear(); }

private:
    // dataCallback: miniaudio realtime callback — reads from the ring into the
    //               output buffer and applies volume/mute.
    //
    // Called by: miniaudio on the audio device thread (the master clock consumer).
    // Calls:     AudioRing::read, fetch_add on consumedTotal_, applies volume/mute.
    // Notes:     Runs on a realtime thread — no locks, no allocations, no logging.
    //               Atomic loads only. Zero-fills on ring underrun.
    static void dataCallback(ma_device* device, void* output, const void* input,
                             uint32_t frameCount);

    std::unique_ptr<AudioRing> ring_;
    std::unique_ptr<ma_device, void(*)(ma_device*)> device_{nullptr, nullptr};
    int channels_ = 2;
    int sampleRate_ = 48000;

    std::atomic<int64_t> startPtsUs_{-1};
    std::atomic<uint64_t> consumedTotal_{0}; // samples consumed
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    mutable std::mutex startMutex_;
};

} // namespace temporal_forge
