// SideBufferSynth.cpp
#include "render/SideBufferSynth.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace temporal_forge {

namespace {

JitterOffset captureJitterSample(JitterSequence sequence, uint32_t index) {
    switch (sequence) {
    case JitterSequence::Halton32:
        return JitterOffset{
            static_cast<float>(halton(index, 3) - 0.5),
            static_cast<float>(halton(index, 2) - 0.5)};
    case JitterSequence::Alternating:
        return (index & 1u) ? JitterOffset{0.25f, -0.25f}
                             : JitterOffset{-0.25f, 0.25f};
    case JitterSequence::Zero:
        return JitterOffset{0.0f, 0.0f};
    case JitterSequence::Halton23:
    default:
        return haltonJitter(index);
    }
}

} // namespace

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
    const JitterOffset j = captureJitterSample(jitterSequence_, jitterIndex_);
    float policyStrength = 0.0f;
    switch (jitterMode_) {
    case JitterMode::Off: policyStrength = 0.0f; break;
    case JitterMode::Current: policyStrength = jitterStrength_; break;
    case JitterMode::Reduced: policyStrength = jitterStrength_ * 0.5f; break;
    case JitterMode::Controlled: policyStrength = controlledJitterStrength_; break;
    }
    const float jitterScale = jitterAmplitudeScale(renderWidth_, renderHeight_) *
                              std::clamp(policyStrength, 0.0f, 1.5f);
    out.jitterX = j.x * jitterScale;
    out.jitterY = j.y * jitterScale;
    ++jitterCadenceCounter_;
    if (jitterCadenceCounter_ >= jitterCadence_) {
        jitterCadenceCounter_ = 0;
        ++jitterIndex_;
        if (jitterIndex_ > 64) jitterIndex_ = 1; // wrap phase
    }

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
    const float observedInterval = ptsDeltaMs > 0.0f
        ? ptsDeltaMs : expectedFrameIntervalMs_;
    const float expectedInterval = expectedIntervalEstablished_
        ? expectedFrameIntervalMs_ : observedInterval;
    out.expectedFrameIntervalMs = expectedInterval;
    out.motionConfidence = std::clamp(motionConfidence, 0.0f, 1.0f);
    const bool sceneCut = shouldReset(out.histogramDelta,
                                      out.motionConfidence,
                                      expectedIntervalEstablished_ ? ptsDeltaMs
                                                                    : 0.0f,
                                      expectedInterval);
    out.reset = forcedReset || sceneCut;

    // --- reactive average (spec 03 section 5) ---
    // The default remains histogram-driven. The opt-in motion-confidence arm
    // adds inverse correspondence confidence so uncertain motion becomes
    // reactive before reaching the hard scene-cut threshold.
    float motionUncertainty = std::clamp(out.histogramDelta, 0.0f, 1.0f);
    if (motionConfidenceReactive_)
        motionUncertainty = std::max(
            motionUncertainty, 1.0f - out.motionConfidence);
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
        if (!expectedIntervalEstablished_) {
            expectedFrameIntervalMs_ = ptsDeltaMs;
            expectedIntervalEstablished_ = true;
        } else if (ptsDeltaMs <= 2.5f * expectedFrameIntervalMs_) {
            // A small EMA follows VFR without allowing one bad gap to
            // disable the discontinuity detector on the next frame.
            expectedFrameIntervalMs_ =
                expectedFrameIntervalMs_ * 0.9f + ptsDeltaMs * 0.1f;
        }
    }
    previousFrameValid_ = true;

    // spec 02: reset jitter index on seek/new-file/scene-cut discontinuity.
    if (out.reset) {
        jitterIndex_ = 1;
        jitterCadenceCounter_ = 0;
    }

    return out;
}

std::vector<MvEntry> SideBufferSynth::estimateFallbackMotion(
    const LumaBuffer& current, uint32_t sourceWidth,
    uint32_t sourceHeight) const {
    // This estimator intentionally works on the already-downsampled analysis
    // image. It is a bounded diagnostic-quality fallback, not a replacement
    // for decoder vectors or a full optical-flow solver. The caller scales
    // the resulting source-pixel vectors into the model grid and the existing
    // GPU uploader expands the sparse blocks into the dense RG16F field.
    if (!previousFrameValid_ || sourceWidth == 0 || sourceHeight == 0 ||
        current.width == 0 || current.height == 0 ||
        previousLuma_.width != current.width ||
        previousLuma_.height != current.height ||
        current.data.size() != static_cast<size_t>(current.width) * current.height ||
        previousLuma_.data.size() !=
            static_cast<size_t>(previousLuma_.width) * previousLuma_.height)
        return {};

    constexpr uint32_t blockSize = 4;
    constexpr int searchRadius = 4;
    const float sourceScaleX = static_cast<float>(sourceWidth) /
                                static_cast<float>(current.width);
    const float sourceScaleY = static_cast<float>(sourceHeight) /
                                static_cast<float>(current.height);
    std::vector<MvEntry> result;
    result.reserve(((current.width + blockSize - 1) / blockSize) *
                   ((current.height + blockSize - 1) / blockSize));

    auto at = [](const LumaBuffer& image, int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.data[static_cast<size_t>(y) * image.width + x];
    };

    for (uint32_t blockY = 0; blockY < current.height;
         blockY += blockSize) {
        const uint32_t blockH =
            std::min(blockSize, current.height - blockY);
        for (uint32_t blockX = 0; blockX < current.width;
             blockX += blockSize) {
            const uint32_t blockW =
                std::min(blockSize, current.width - blockX);
            float bestSad = std::numeric_limits<float>::infinity();
            int bestDx = 0;
            int bestDy = 0;
            for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
                for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
                    float sad = 0.0f;
                    for (uint32_t y = 0; y < blockH; ++y) {
                        for (uint32_t x = 0; x < blockW; ++x) {
                            sad += std::abs(
                                at(current, static_cast<int>(blockX + x),
                                   static_cast<int>(blockY + y)) -
                                at(previousLuma_,
                                   static_cast<int>(blockX + x) + dx,
                                   static_cast<int>(blockY + y) + dy));
                        }
                    }
                    // Keep the zero-displacement candidate on exact ties.
                    // Flat or repeating regions otherwise select the first
                    // search coordinate, creating arbitrary motion in static
                    // content even though every candidate has equal error.
                    const int bestDistance = std::abs(bestDx) + std::abs(bestDy);
                    const int candidateDistance = std::abs(dx) + std::abs(dy);
                    if (sad < bestSad - 1e-7f ||
                        (std::abs(sad - bestSad) <= 1e-7f &&
                         candidateDistance < bestDistance)) {
                        bestSad = sad;
                        bestDx = dx;
                        bestDy = dy;
                    }
                }
            }

            MvEntry motion;
            motion.dstX = static_cast<int16_t>(std::clamp(
                static_cast<long>(std::lround(blockX * sourceScaleX)),
                -32768L, 32767L));
            motion.dstY = static_cast<int16_t>(std::clamp(
                static_cast<long>(std::lround(blockY * sourceScaleY)),
                -32768L, 32767L));
            motion.mvX = static_cast<float>(bestDx) * sourceScaleX;
            motion.mvY = static_cast<float>(bestDy) * sourceScaleY;
            motion.w = static_cast<uint8_t>(std::clamp(
                static_cast<long>(std::lround(blockW * sourceScaleX)), 1L,
                255L));
            motion.h = static_cast<uint8_t>(std::clamp(
                static_cast<long>(std::lround(blockH * sourceScaleY)), 1L,
                255L));
            motion.source = 0;
            result.push_back(motion);
        }
    }
    return result;
}

std::vector<MvEntry> SideBufferSynth::refineCodecMotion(
    const LumaBuffer& current,
    const std::vector<MvEntry>& seeds,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    int refinementRadius) const {
    // The decoder vector remains the prior. This pass only nudges it when a
    // nearby analysis-luma position matches measurably better. Keeping the
    // search around the codec answer preserves reference direction semantics
    // while correcting block quantization and small codec-vector errors.
    if (!previousFrameValid_ || previousLuma_.data.empty() ||
        current.data.empty() || current.width == 0 || current.height == 0 ||
        sourceWidth == 0 || sourceHeight == 0 || seeds.empty()) {
        return seeds;
    }

    const int radius = std::clamp(refinementRadius, 0, 2);
    if (radius == 0) return seeds;
    const float analysisScaleX = static_cast<float>(current.width) /
                                 static_cast<float>(sourceWidth);
    const float analysisScaleY = static_cast<float>(current.height) /
                                 static_cast<float>(sourceHeight);
    if (!std::isfinite(analysisScaleX) || !std::isfinite(analysisScaleY) ||
        analysisScaleX <= 0.0f || analysisScaleY <= 0.0f) {
        return seeds;
    }

    auto at = [](const LumaBuffer& image, int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.data[static_cast<size_t>(y) * image.width + x];
    };

    // A 3x3 patch is enough to reject a one-pixel block-vector error while
    // keeping this work several orders cheaper than dense optical flow.
    const auto localSad = [&](int x, int y, int dx, int dy, int halfW,
                              int halfH) {
        float sad = 0.0f;
        int samples = 0;
        for (int oy = -halfH; oy <= halfH; ++oy) {
            for (int ox = -halfW; ox <= halfW; ++ox) {
                sad += std::abs(at(current, x + ox, y + oy) -
                                at(previousLuma_, x + dx + ox,
                                   y + dy + oy));
                ++samples;
            }
        }
        return samples > 0 ? sad / static_cast<float>(samples) : 1.0f;
    };

    std::vector<MvEntry> refined = seeds;
    for (size_t i = 0; i < seeds.size(); ++i) {
        const MvEntry& seed = seeds[i];
        if (seed.source > 0 || !std::isfinite(seed.mvX) ||
            !std::isfinite(seed.mvY)) {
            continue;
        }
        const int x = std::clamp(static_cast<int>(std::lround(
                                      (static_cast<float>(seed.dstX) +
                                       static_cast<float>(seed.w) * 0.5f) *
                                      analysisScaleX)),
                                 0, static_cast<int>(current.width) - 1);
        const int y = std::clamp(static_cast<int>(std::lround(
                                      (static_cast<float>(seed.dstY) +
                                       static_cast<float>(seed.h) * 0.5f) *
                                      analysisScaleY)),
                                 0, static_cast<int>(current.height) - 1);
        const int seedDx = static_cast<int>(std::lround(seed.mvX * analysisScaleX));
        const int seedDy = static_cast<int>(std::lround(seed.mvY * analysisScaleY));
        const int halfW = std::clamp(static_cast<int>(std::lround(
                                           static_cast<float>(std::max(1, static_cast<int>(seed.w))) *
                                           analysisScaleX * 0.25f)), 1, 2);
        const int halfH = std::clamp(static_cast<int>(std::lround(
                                           static_cast<float>(std::max(1, static_cast<int>(seed.h))) *
                                           analysisScaleY * 0.25f)), 1, 2);
        const float seedSad = localSad(x, y, seedDx, seedDy, halfW, halfH);
        float bestSad = seedSad;
        int bestDx = seedDx;
        int bestDy = seedDy;
        for (int dy = seedDy - radius; dy <= seedDy + radius; ++dy) {
            for (int dx = seedDx - radius; dx <= seedDx + radius; ++dx) {
                const float sad = localSad(x, y, dx, dy, halfW, halfH);
                const int bestDistance = std::abs(bestDx - seedDx) +
                                         std::abs(bestDy - seedDy);
                const int candidateDistance = std::abs(dx - seedDx) +
                                              std::abs(dy - seedDy);
                if (sad < bestSad - 1e-5f ||
                    (std::abs(sad - bestSad) <= 1e-5f &&
                     candidateDistance < bestDistance)) {
                    bestSad = sad;
                    bestDx = dx;
                    bestDy = dy;
                }
            }
        }
        // Ignore noise-level wins. This is important in flat or compressed
        // regions where many nearby vectors are equally plausible.
        if (bestSad < seedSad - 0.0025f) {
            refined[i].mvX += static_cast<float>(bestDx - seedDx) /
                              analysisScaleX;
            refined[i].mvY += static_cast<float>(bestDy - seedDy) /
                              analysisScaleY;
        }
    }
    return refined;
}

} // namespace temporal_forge
