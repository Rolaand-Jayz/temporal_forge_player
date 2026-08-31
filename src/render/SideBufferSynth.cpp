// SideBufferSynth.cpp
#include "render/SideBufferSynth.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// Read the fallback matcher radius only for explicitly requested diagnostics.
// Upstream: the capture environment. Downstream: the bounded causal search
// in estimateFallbackMotion. Keeping the absent/invalid value at four pixels
// preserves the normal playback path and its established CPU cost.
int fallbackMotionSearchRadius() {
    constexpr int defaultRadius = 4;
    constexpr int maximumRadius = 12;
    const char* value =
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS");
    if (!value || !*value) return defaultRadius;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return defaultRadius;
    return std::clamp(static_cast<int>(parsed), 1, maximumRadius);
}

// Select only the explicitly supported diagnostic block geometries. Keeping
// the default at 4x4 preserves the established vector density and CPU cost;
// larger blocks are intended for controlled compressed-video experiments.
uint32_t fallbackMotionBlockSize() {
    const char* value =
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_BLOCK_SIZE");
    if (!value || !*value) return 4;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end != value && *end == '\0' && (parsed == 8 || parsed == 16))
        return static_cast<uint32_t>(parsed);
    return 4;
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
                                  float expectedFrameIntervalMs,
                                  float histogramThreshold) {
    // spec 03 section 8 scene-cut detection.
    if (histogramDelta > std::clamp(histogramThreshold, 0.0f, 1.0f)) return true;
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

    // Snapshot every state component that update() may advance. The caller
    // prepares side inputs before dispatch, but only a successful FSR chain
    // is allowed to publish a new temporal frame. This transaction pairs the
    // CPU analysis history with the GPU history commit/rollback boundary.
    previousLumaBeforeUpdate_ = previousLuma_;
    previousAvgLumaBeforeUpdate_ = previousAvgLuma_;
    previousHistBeforeUpdate_ = previousHist_;
    previousFrameValidBeforeUpdate_ = previousFrameValid_;
    expectedIntervalEstablishedBeforeUpdate_ = expectedIntervalEstablished_;
    expectedFrameIntervalBeforeUpdate_ = expectedFrameIntervalMs_;

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
                                      expectedInterval,
                                      sceneCutThreshold_);
    out.reset = forcedReset || sceneCut;

    // --- jitter ---
    // Reset before selecting the sample. This keeps the first color sample
    // after a seek or scene cut aligned with the freshly reset FSR history.
    if (out.reset) {
        jitterIndex_ = 1;
        jitterCadenceCounter_ = 0;
    }
    jitterIndexBeforeUpdate_ = jitterIndex_;
    jitterCadenceCounterBeforeUpdate_ = jitterCadenceCounter_;
    jitterRollbackValid_ = true;
    const JitterOffset j = captureJitterSample(jitterSequence_, jitterIndex_);
    float policyStrength = 0.0f;
    switch (jitterMode_) {
    case JitterMode::Off: policyStrength = 0.0f; break;
    case JitterMode::Current: policyStrength = jitterStrength_; break;
    case JitterMode::Reduced: policyStrength = jitterStrength_ * 0.5f; break;
    case JitterMode::Controlled: policyStrength = controlledJitterStrength_; break;
    }
    // The full-amplitude branch is a diagnostic comparison against the FSR
    // render-pixel contract. It changes only jitter magnitude; sequence,
    // cadence, history, motion, and reset handling remain identical.
    const float amplitudeScale = fullAmplitudeJitter_
        ? 1.0f
        : jitterAmplitudeScale(renderWidth_, renderHeight_);
    const float jitterScale = amplitudeScale *
                              std::clamp(policyStrength, 0.0f, 1.5f);
    out.jitterX = j.x * jitterScale;
    out.jitterY = j.y * jitterScale;
    ++jitterCadenceCounter_;
    if (jitterCadenceCounter_ >= jitterCadence_) {
        jitterCadenceCounter_ = 0;
        ++jitterIndex_;
        const uint32_t phaseCount = fsrJitterPhaseCount(
            phaseRenderWidth_ != 0 ? phaseRenderWidth_ : renderWidth_,
            phasePresentationWidth_ != 0
                ? phasePresentationWidth_
                : (presentationWidth_ != 0
                       ? presentationWidth_
                       : (phaseRenderWidth_ != 0 ? phaseRenderWidth_
                                                 : renderWidth_)));
        if (jitterIndex_ > phaseCount) jitterIndex_ = 1;
    }

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

    return out;
}

// rollbackJitter: undo the phase/cadence and analysis-history mutations made
// by the latest update() call when its prepared frame never reaches FSR. The
// method name is retained because it is the existing frame-abort boundary
// used by PlaybackEngine. A reset boundary starts again at phase one.
void SideBufferSynth::rollbackJitter(bool resetPhase) {
    if (!jitterRollbackValid_)
        return;
    if (resetPhase) {
        jitterIndex_ = 1;
        jitterCadenceCounter_ = 0;
    } else {
        jitterIndex_ = jitterIndexBeforeUpdate_;
        jitterCadenceCounter_ = jitterCadenceCounterBeforeUpdate_;
    }
    previousLuma_ = previousLumaBeforeUpdate_;
    previousAvgLuma_ = previousAvgLumaBeforeUpdate_;
    previousHist_ = previousHistBeforeUpdate_;
    previousFrameValid_ = previousFrameValidBeforeUpdate_;
    expectedIntervalEstablished_ = expectedIntervalEstablishedBeforeUpdate_;
    expectedFrameIntervalMs_ = expectedFrameIntervalBeforeUpdate_;
    jitterRollbackValid_ = false;
}

// resetAnalysisHistory: clear all source-frame state that must not cross a
// decoder flush or file switch. GPU FSR images are reset by the caller; this
// clears the CPU-side luma/cadence/jitter companion state as well.
void SideBufferSynth::resetAnalysisHistory() {
    previousLuma_ = {};
    previousAvgLuma_ = -1.0f;
    previousHist_.fill(0);
    previousFrameValid_ = false;
    expectedIntervalEstablished_ = false;
    expectedFrameIntervalMs_ = 16.6667f;
    jitterIndex_ = 1;
    jitterCadenceCounter_ = 0;
    jitterRollbackValid_ = false;
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

    const uint32_t blockSize = fallbackMotionBlockSize();
    const bool coarseToFine =
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_COARSE_TO_FINE") !=
        nullptr;
    // Coarse-to-fine is itself an explicit diagnostic mode. When it is used
    // without a radius override, give it the wider twelve-pixel envelope it
    // exists to investigate; an explicit radius remains authoritative.
    const int searchRadius =
        coarseToFine &&
                !std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS")
            ? 12
            : fallbackMotionSearchRadius();
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
            const auto consider = [&](int dx, int dy) {
                float sad = 0.0f;
                for (uint32_t y = 0; y < blockH; ++y) {
                    for (uint32_t x = 0; x < blockW; ++x) {
                        sad += std::abs(
                            at(current, static_cast<int>(blockX + x),
                               static_cast<int>(blockY + y)) -
                            at(previousLuma_, static_cast<int>(blockX + x) + dx,
                               static_cast<int>(blockY + y) + dy));
                    }
                }
                // Keep the zero-displacement candidate on exact ties. Flat
                // regions otherwise select an arbitrary search coordinate.
                const int bestDistance = std::abs(bestDx) + std::abs(bestDy);
                const int candidateDistance = std::abs(dx) + std::abs(dy);
                if (sad < bestSad - 1e-7f ||
                    (std::abs(sad - bestSad) <= 1e-7f &&
                     candidateDistance < bestDistance)) {
                    bestSad = sad;
                    bestDx = dx;
                    bestDy = dy;
                }
            };
            if (!coarseToFine) {
                for (int dy = -searchRadius; dy <= searchRadius; ++dy)
                    for (int dx = -searchRadius; dx <= searchRadius; ++dx)
                        consider(dx, dy);
            } else {
                // Sample every second displacement over the wide envelope,
                // then inspect the 3x3 neighborhood around its winner. This
                // retains exact one-pixel recovery while avoiding the
                // quadratic cost of a full large-radius sweep.
                for (int dy = -searchRadius; dy <= searchRadius; dy += 2)
                    for (int dx = -searchRadius; dx <= searchRadius; dx += 2)
                        consider(dx, dy);
                const int fineMinY = std::max(-searchRadius, bestDy - 1);
                const int fineMaxY = std::min(searchRadius, bestDy + 1);
                const int fineMinX = std::max(-searchRadius, bestDx - 1);
                const int fineMaxX = std::min(searchRadius, bestDx + 1);
                for (int dy = fineMinY; dy <= fineMaxY; ++dy)
                    for (int dx = fineMinX; dx <= fineMaxX; ++dx)
                        consider(dx, dy);
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
            // Unlike an FFmpeg codec vector, this matcher compares the
            // current decoded image directly against the previous decoded
            // image retained by this synthesizer. Mark it as the known causal
            // direction so the shared validation/scoring stages do not treat
            // it as an ambiguous codec reference and discard it.
            motion.source = -1;
            // A block matcher is only a fallback correspondence source. Use
            // its normalized photometric residual as the initial trust score
            // instead of inheriting MvEntry's full-confidence default; the
            // downstream validity texture can then reject ambiguous blocks
            // conservatively when the match is poor.
            const float meanSad = bestSad / static_cast<float>(
                std::max<uint32_t>(1u, blockW * blockH));
            motion.confidence = std::clamp(
                std::exp(-meanSad / 0.04f), 0.0f, 1.0f);
            result.push_back(motion);
        }
    }
    return result;
}

std::vector<MvEntry> SideBufferSynth::fuseBidirectionalMotion(
    const std::vector<MvEntry>& past, const std::vector<MvEntry>& future,
    float consistencyThreshold) {
    // No future estimate must never erase the causal field. This makes the
    // helper safe at end-of-stream and on a decoder that cannot expose a next
    // frame, while the caller remains responsible for opting into this path.
    if (past.empty() || future.empty()) return past;

    std::vector<MvEntry> result = past;
    for (MvEntry& causal : result) {
        const MvEntry* nearest = nullptr;
        int nearestDistance = std::numeric_limits<int>::max();
        for (const MvEntry& candidate : future) {
            const int dx = std::abs(static_cast<int>(causal.dstX) -
                                    static_cast<int>(candidate.dstX));
            const int dy = std::abs(static_cast<int>(causal.dstY) -
                                    static_cast<int>(candidate.dstY));
            const int allowedX = std::max<int>(causal.w, candidate.w);
            const int allowedY = std::max<int>(causal.h, candidate.h);
            if (dx > allowedX || dy > allowedY) continue;
            const int distance = dx + dy;
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = &candidate;
            }
        }
        if (!nearest) continue;

        const float threshold = consistencyThreshold > 0.0f
                                    ? consistencyThreshold
                                    : std::max(2.0f, std::hypot(
                                                         static_cast<float>(causal.w),
                                                         static_cast<float>(causal.h)) *
                                                         0.75f);
        const float disagreement = std::hypot(causal.mvX - nearest->mvX,
                                              causal.mvY - nearest->mvY);
        if (disagreement <= threshold) {
            causal.mvX = (causal.mvX + nearest->mvX) * 0.5f;
            causal.mvY = (causal.mvY + nearest->mvY) * 0.5f;
            causal.confidence = std::clamp(
                std::min(causal.confidence, nearest->confidence) *
                    std::exp(-disagreement / threshold),
                0.0f, 1.0f);
        } else {
            // Preserve the causal vector but tell the existing validity path
            // that the next-frame evidence disagrees with it.
            causal.confidence = std::clamp(causal.confidence * 0.25f, 0.0f, 1.0f);
        }
    }
    return result;
}

std::vector<MvEntry> SideBufferSynth::gateMotionWithFutureEvidence(
    const std::vector<MvEntry>& past, const std::vector<MvEntry>& future,
    float consistencyThreshold) {
    // Future evidence is a confidence signal only. Do not average the older
    // bidirectional probe's vectors: these estimates span different frame
    // pairs, so changing mvX/mvY would change reprojection semantics.
    if (past.empty() || future.empty()) return past;

    std::vector<MvEntry> result = past;
    for (MvEntry& causal : result) {
        const MvEntry* nearest = nullptr;
        float nearestDistance = std::numeric_limits<float>::infinity();
        for (const MvEntry& candidate : future) {
            // estimateFallbackMotion stores a future->current vector. Map
            // the future block back to current coordinates before matching
            // it against the causal current->previous destination. Matching
            // raw destinations would select the wrong block during motion.
            const float projectedX = static_cast<float>(candidate.dstX) +
                                     candidate.mvX;
            const float projectedY = static_cast<float>(candidate.dstY) +
                                     candidate.mvY;
            const float dx = std::abs(static_cast<float>(causal.dstX) -
                                      projectedX);
            const float dy = std::abs(static_cast<float>(causal.dstY) -
                                      projectedY);
            if (dx > static_cast<float>(std::max<int>(causal.w, candidate.w)) ||
                dy > static_cast<float>(std::max<int>(causal.h, candidate.h)))
                continue;
            const float distance = dx + dy;
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = &candidate;
            }
        }
        if (!nearest) continue;

        const float threshold = consistencyThreshold > 0.0f
                                    ? consistencyThreshold
                                    : std::max(2.0f, std::hypot(
                                                         static_cast<float>(causal.w),
                                                         static_cast<float>(causal.h)) *
                                                         0.75f);
        const float disagreement = std::hypot(causal.mvX - nearest->mvX,
                                              causal.mvY - nearest->mvY);
        if (disagreement > threshold) {
            // Preserve the causal reprojection and lower only history trust.
            causal.confidence = std::clamp(
                causal.confidence * std::exp(-disagreement / threshold),
                0.0f, 1.0f);
        }
    }
    return result;
}

std::vector<MvEntry> SideBufferSynth::refineCodecMotion(
    const LumaBuffer& current,
    const std::vector<MvEntry>& seeds,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    int refinementRadius,
    float maxCorrectionPixels) const {
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
        if (seed.source != -1 || !std::isfinite(seed.mvX) ||
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
        // Ignore noise-level wins and source-space jumps that the reduced
        // analysis grid cannot represent reliably. The latter is especially
        // important for a valid fractional codec seed such as -2 at quarter
        // resolution, where one integer analysis step is four source pixels.
        const float correctionX =
            static_cast<float>(bestDx - seedDx) / analysisScaleX;
        const float correctionY =
            static_cast<float>(bestDy - seedDy) / analysisScaleY;
        if (bestSad < seedSad - 0.0025f &&
            std::hypot(correctionX, correctionY) <=
                std::max(0.0f, maxCorrectionPixels)) {
            refined[i].mvX += correctionX;
            refined[i].mvY += correctionY;
        } else if (bestSad < seedSad - 0.0025f) {
            // Preserve the decoder vector but lower trust when the only
            // apparent improvement requires an implausibly large jump.
            refined[i].confidence *= 0.5f;
        }
    }
    return refined;
}

std::vector<MvEntry> SideBufferSynth::validateCodecMotion(
    const LumaBuffer& current,
    const std::vector<MvEntry>& seeds,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    float maxPatchError) const {
    // This bounded CPU check is deliberately separate from refinement. It
    // answers only whether a seed is trustworthy enough for history; it never
    // invents a replacement vector and therefore preserves decoder semantics.
    if (!previousFrameValid_ || previousLuma_.data.empty() ||
        current.data.empty() || current.width == 0 || current.height == 0 ||
        sourceWidth == 0 || sourceHeight == 0 || seeds.empty() ||
        !std::isfinite(maxPatchError) || maxPatchError < 0.0f) {
        return seeds;
    }
    const float analysisScaleX = static_cast<float>(current.width) /
                                 static_cast<float>(sourceWidth);
    const float analysisScaleY = static_cast<float>(current.height) /
                                 static_cast<float>(sourceHeight);
    if (!std::isfinite(analysisScaleX) || !std::isfinite(analysisScaleY) ||
        analysisScaleX <= 0.0f || analysisScaleY <= 0.0f)
        return seeds;

    auto at = [](const LumaBuffer& image, int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.data[static_cast<size_t>(y) * image.width + x];
    };
    const auto patchSad = [&](int x, int y, int dx, int dy) {
        float error = 0.0f;
        constexpr int halfExtent = 1;
        for (int oy = -halfExtent; oy <= halfExtent; ++oy) {
            for (int ox = -halfExtent; ox <= halfExtent; ++ox) {
                error += std::abs(at(current, x + ox, y + oy) -
                                  at(previousLuma_, x + dx + ox, y + dy + oy));
            }
        }
        return error / 9.0f;
    };

    std::vector<MvEntry> validated;
    validated.reserve(seeds.size());
    for (const MvEntry& seed : seeds) {
        // Only FFmpeg's -1 marker denotes the immediately previous
        // displayed frame in this causal history path. Zero is ambiguous
        // (often intra/skip/no-reference), and values below -1 refer to
        // older pictures; neither may influence current-frame reprojection.
        if (seed.source != -1 || !std::isfinite(seed.mvX) ||
            !std::isfinite(seed.mvY))
            continue;
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
        const int dx = static_cast<int>(std::lround(seed.mvX * analysisScaleX));
        const int dy = static_cast<int>(std::lround(seed.mvY * analysisScaleY));
        if (patchSad(x, y, dx, dy) <= maxPatchError)
            validated.push_back(seed);
    }
    return validated;
}

std::vector<MvEntry> SideBufferSynth::scoreCodecMotion(
    const LumaBuffer& current,
    const std::vector<MvEntry>& seeds,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    float errorScale) const {
    // Confidence is a soft trust score, not a new motion estimate. Keeping
    // every seed lets the prepass reduce history smoothly instead of creating
    // block-shaped holes at an arbitrary threshold.
    if (!previousFrameValid_ || previousLuma_.data.empty() ||
        current.data.empty() || current.width == 0 || current.height == 0 ||
        sourceWidth == 0 || sourceHeight == 0 || seeds.empty() ||
        !std::isfinite(errorScale) || errorScale <= 0.0f)
        return seeds;
    const float analysisScaleX = static_cast<float>(current.width) /
                                 static_cast<float>(sourceWidth);
    const float analysisScaleY = static_cast<float>(current.height) /
                                 static_cast<float>(sourceHeight);
    if (!std::isfinite(analysisScaleX) || !std::isfinite(analysisScaleY) ||
        analysisScaleX <= 0.0f || analysisScaleY <= 0.0f)
        return seeds;

    auto at = [](const LumaBuffer& image, int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.data[static_cast<size_t>(y) * image.width + x];
    };
    const auto patchSad = [&](int x, int y, int dx, int dy) {
        float error = 0.0f;
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox)
                error += std::abs(at(current, x + ox, y + oy) -
                                  at(previousLuma_, x + dx + ox, y + dy + oy));
        return error / 9.0f;
    };

    std::vector<MvEntry> scored = seeds;
    for (MvEntry& seed : scored) {
        // Keep confidence scoring under the same causal reference boundary
        // as validation and refinement. Scoring an older or ambiguous vector
        // would make it appear trustworthy to the downstream history pass.
        if (seed.source != -1 || !std::isfinite(seed.mvX) ||
            !std::isfinite(seed.mvY)) {
            seed.confidence = 0.0f;
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
        const int dx = static_cast<int>(std::lround(seed.mvX * analysisScaleX));
        const int dy = static_cast<int>(std::lround(seed.mvY * analysisScaleY));
        const float error = patchSad(x, y, dx, dy);
        seed.confidence = std::clamp(std::exp(-error / errorScale), 0.0f, 1.0f);
    }
    return scored;
}

} // namespace temporal_forge
