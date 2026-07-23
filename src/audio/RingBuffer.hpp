// RingBuffer.hpp — single-producer single-consumer lock-free ring buffer.
// Used by the audio path: the audio decode thread (producer) writes float
// samples, and the audio device callback (consumer, realtime thread) reads
// them. spec 01: "audio clock" is the primary clock; this buffer feeds it.
//
// Implementation: power-of-two capacity, head/tail indices with
// acquire/release fences. One producer, one consumer.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace temporal_forge {

class AudioRing {
public:
    // AudioRing ctor: allocate a power-of-two-capacity ring buffer.
    //
    // Called by: AudioSink ctor (capacity ~1s of stereo float @ 48k, rounded
    //            up to 256k samples).
    // Notes:     Capacity is rounded up to the next power of two so the read/write
    //            indices can use a bitmask instead of a modulo.
    explicit AudioRing(size_t capacitySamples) {
        // round up to power of two
        size_t cap = 1;
        while (cap < capacitySamples) cap <<= 1;
        buf_.resize(cap);
        cap_ = cap;
        mask_ = cap - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // write: producer side — write up to n interleaved float samples.
    //
    // Called by: AudioSink::push (audio decode thread, the producer).
    // Calls:     head_/tail_ with release fence.
    // Returns:   count actually written (may be < n if the ring is full).
    // Notes:     One producer only (the audio decode thread). Lock-free via
    //            acquire/release fences on head_/tail_.
    size_t write(const float* src, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free = cap_ - (head - tail);
        const size_t count = n < free ? n : free;
        for (size_t i = 0; i < count; ++i)
            buf_[(head + i) & mask_] = src[i];
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // read: consumer side — read up to n samples, zero-filling any shortfall.
    //
    // Called by: AudioSink::dataCallback (the realtime miniaudio device thread,
    //            the consumer — this is the master clock).
    // Calls:     head_/tail_ with acquire/release fences.
    // Returns:   count actually read (may be < n; caller writes silence for rest).
    // Notes:     One consumer only (the audio device callback). Zero-fill on
    //            shortfall so the audio device never starves — a gap here would
    //            cause a click and a clock jump.
    size_t read(float* dst, size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t avail = head - tail;
        const size_t count = n < avail ? n : avail;
        for (size_t i = 0; i < count; ++i)
            dst[i] = buf_[(tail + i) & mask_];
        // Zero-fill remaining so output is well-defined.
        for (size_t i = count; i < n; ++i) dst[i] = 0.0f;
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // available: number of samples currently buffered (head - tail).
    //            Called by: pacing logic that checks buffer health.
    size_t available() const {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }
    // capacity: total ring capacity in samples.
    size_t capacity() const { return cap_; }

    // clear: reset head/tail to zero (empties the ring).
    //        Called by: AudioSink::start (on device reconfig) and AudioSink::clear.
    //        Notes:     tail_ store uses release so the next consumer read sees
    //                   the cleared state.
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_release);
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace temporal_forge
