// SideBufferSynth.cpp
#include "render/SideBufferSynth.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cmath>

namespace temporal_forge {

SideBufferSynth::SideBufferSynth() {
    previousHist_.fill(0);
}

// computeLumaStats: build a 64-bin luma histogram + average for a frame.
//                     Called by: SideBufferSynth::update each frame.
LumaStats computeLumaStats(const LumaBuffer& luma) {
    LumaStats s;
    s.hist.fill(0);
    if (luma.data.empty()) return s;
    double sum = 0.0;
    for (float v : luma.data) {
        v = std::clamp(v, 0.0f, 1.0f);
        sum += v;
        // 64 bins over [0,1].
        uint32_t bin = std::min<uint32_t>(63u, static_cast<uint32_t>(v * 64.0f));
        s.hist[bin]++;
    }
    s.average = static_cast<float>(sum / luma.data.size());
    return s;
}

// histogramDelta: 1 - intersection/union over two 64-bin histograms, in [0,1].
//                  Called by: SideBufferSynth::update (frame-to-frame delta
//                  feeds the scene-cut detector). 0 = identical, 1 = disjoint.
float histogramDelta(const std::array<uint32_t, 64>& a,
                     const std::array<uint32_t, 64>& b) {
    // 1 - intersection/union over the 64 bins. Bounded to [0,1].
    uint64_t inter = 0, uni = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        inter += std::min(a[i], b[i]);
        uni += std::max(a[i], b[i]);
    }
    if (uni == 0) return 0.0f;
    return 1.0f - static_cast<float>(static_cast<double>(inter) / uni);
}

// reactiveValue: weighted blend of luma/motion/edge change into a reactive
//                 mask value (spec 03 section 5), clamped per mode.
//                 Called by: update to fill the reactive side buffer.
float SideBufferSynth::reactiveValue(float lumaDifference,
                                     float motionUncertainty,
                                     float edgeChange,
                                     ReactiveSynthMode mode) {
    // spec 03 section 5 formula.
    const float cap = (mode == ReactiveSynthMode::Aggressive) ? 0.9f : 0.85f;
    float v = lumaDifference * 0.60f
            + motionUncertainty * 0.30f
            + edgeChange * 0.10f;
    return std::clamp(v, 0.0f, cap);
}

// shouldReset: spec 03 section 8 scene-cut detector — returns true when a
//               temporal history reset is warranted (large histogram delta,
//               low motion confidence, or a PTS gap > 2.5x the frame interval).
//               Called by: update.
bool SideBufferSynth::shouldReset(float histogramDelta,
                                  float motionConfidence,
                                  float ptsGapMs,
                                  float expectedFrameIntervalMs) {
    // spec 03 section 8 scene-cut detection.
    if (histogramDelta > 0.65f) return true;
    if (motionConfidence < 0.15f) return true;
    if (expectedFrameIntervalMs > 0.0f &&
        ptsGapMs > 2.5f * expectedFrameIntervalMs) return true;
    return false;
}

// update: produce the per-frame side-buffer inputs (jitter offset, reactive
//         mask, depth, reset flag) from the current luma buffer + PTS delta.
//
// Called by: PlaybackEngine::videoDecodeLoop each frame (before dispatch).
// Calls:    haltonJitter, jitterAmplitudeScale, computeLumaStats, histogramDelta,
//           shouldReset (scene-cut detection), reactiveValue.
// Notes:    Maintains the previous-frame histogram/average for delta computation;
//           forcedReset (seek/new-file) bypasses the detector and resets anyway.
//           motionConfidence is supplied by the decoder's codec-vector
//           analysis; it is deliberately conservative when vectors are sparse.
SideBufferInputs SideBufferSynth::update(const LumaBuffer& lumaCurrent,
                                         float ptsDeltaMs,
                                         bool forcedReset,
                                         float motionConfidence) {
    SideBufferInputs out;

    // --- jitter ---
    const JitterOffset j = haltonJitter(jitterIndex_);
    const float jitterScale = jitterAmplitudeScale(renderWidth_, renderHeight_) * jitterStrength_;
    out.jitterX = j.x * jitterScale;
    out.jitterY = j.y * jitterScale;
    jitterIndex_++;
    if (jitterIndex_ > 64) jitterIndex_ = 1; // wrap phase

    // --- luma stats ---
    const LumaStats cur = computeLumaStats(lumaCurrent);
    out.histogramDelta = histogramDelta(cur.hist, previousHist_);
    out.avgLumaDelta = (previousAvgLuma_ >= 0.0f)
        ? std::abs(cur.average - previousAvgLuma_) : 0.0f;

    // --- scene-cut / reset (spec 03 section 8) ---
    // Keep a stable cadence estimate. Using the current delta as both the
    // observed gap and the expected interval makes a dropped/torn timestamp
    // impossible to detect (the gap can never exceed itself). The first
    // valid frame establishes the estimate; later normal deltas adapt it
    // slowly, while a large gap is deliberately not folded into the estimate.
    const float observedInterval = ptsDeltaMs > 0.0f ? ptsDeltaMs : expectedFrameIntervalMs_;
    const float expectedInterval = previousFrameValid_
        ? expectedFrameIntervalMs_ : observedInterval;
    out.motionConfidence = std::clamp(motionConfidence, 0.0f, 1.0f);
    const bool sceneCut = shouldReset(out.histogramDelta,
                                      out.motionConfidence,
                                      previousFrameValid_ ? ptsDeltaMs : 0.0f,
                                      expectedInterval);
    out.reset = forcedReset || sceneCut;

    // --- reactive average (spec 03 section 5) ---
    // Without real motion vectors yet, motionUncertainty is driven by the
    // histogram delta (more change -> less trust in history).
    float motionUncertainty = std::clamp(out.histogramDelta, 0.0f, 1.0f);
    float edgeChange = std::clamp(out.avgLumaDelta * 4.0f, 0.0f, 1.0f);
    out.reactiveAverage = reactiveValue(out.avgLumaDelta,
                                        motionUncertainty,
                                        edgeChange,
                                        reactiveMode_);

    // --- update previous buffers ---
    previousLuma_ = lumaCurrent;
    previousHist_ = cur.hist;
    previousAvgLuma_ = cur.average;
    if (ptsDeltaMs > 0.0f) {
        if (!previousFrameValid_) {
            expectedFrameIntervalMs_ = ptsDeltaMs;
        } else if (ptsDeltaMs <= 2.5f * expectedFrameIntervalMs_) {
            // A small EMA follows VFR without allowing one bad gap to
            // disable the discontinuity detector on the next frame.
            expectedFrameIntervalMs_ =
                expectedFrameIntervalMs_ * 0.9f + ptsDeltaMs * 0.1f;
        }
    }
    previousFrameValid_ = true;

    // spec 02: reset jitter index on seek/new-file/scene-cut discontinuity.
    if (out.reset) jitterIndex_ = 1;

    return out;
}

} // namespace temporal_forge
