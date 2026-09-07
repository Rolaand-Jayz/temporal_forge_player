// MotionEstimator.hpp — cheap causal correspondence for prerecorded video.
//
// Upstream: VideoDecoder supplies filtered FFmpeg codec blocks and
// SideBufferSynth supplies the small analysis-luma pair. Downstream:
// PlaybackEngine sends the returned source-pixel blocks through the existing
// model-coverage adapter and GPU RG16F expander into the unchanged FSR path.
//
// This class owns estimator policy and statistics; it does not own Vulkan
// images and cannot change FSR's motion-vector contract. Keeping that boundary
// explicit lets the GPU uploader remain the only FSR-facing adapter.
#pragma once

#include "media/VideoDecoder.hpp"
#include "render/SideBufferSynth.hpp"

#include <cstdint>
#include <vector>

namespace temporal_forge {

enum class MotionEstimatorMode { Off, Codec, CodecRefined };

struct MotionEstimatorConfig {
    MotionEstimatorMode mode = MotionEstimatorMode::Off;
    // Analysis scale is expressed as a divisor of source dimensions.
    // 4 means the default quarter-resolution search grid.
    uint32_t refinementScale = 4;
    int searchRadius = 2;
    // Maximum accepted correction measured in source pixels. The default
    // quarter-resolution grid quantizes one integer search step to four source
    // pixels; keeping the default below that would make every nonzero
    // refinement impossible. Tighter bounds remain available through config.
    float maxCorrectionPixels = 4.0f;
    // Minimum normalized SAD improvement required before a local candidate
    // may replace the codec seed. This prevents noise-level wins from
    // changing temporal correspondence.
    float minErrorImprovement = 0.0025f;
    // Minimum normalized SAD margin between the best and second-best local
    // candidates. A small margin means the reduced grid cannot distinguish
    // the alternatives reliably, so the seed is retained.
    float minErrorMargin = 0.001f;
    // Maximum number of codec blocks that receive the expensive local search
    // per frame. Every codec block is still returned unchanged when it is not
    // sampled, so this bounds CPU work without creating coverage holes.
    uint32_t maxRefinedSeeds = 4096;
    float confidenceErrorScale = 0.04f;
    float confidenceThreshold = 0.15f;
    float sceneCutThreshold = 0.65f;
    // When enabled, a refined profile may use the robust global fallback if
    // codec metadata exists but every entry is non-causal or malformed. The
    // baseline keeps the historical empty-field behavior for A/B fidelity.
    bool allowFallbackAfterFiltering = false;
    // Opt-in low-resolution grid that can discover motion where codec blocks
    // are absent. The established seed-only estimator remains the default
    // until real-video quality and CPU cost are measured.
    bool denseGridFallback = false;
    // Preserve the established sparse expansion by default. The boundary-
    // aware GPU resolve is an explicit diagnostic opt-in until it has a
    // scene-diverse quality win.
    bool edgeAwareUpscale = false;
};

struct MotionEstimatorStats {
    uint32_t inputSeeds = 0;
    uint32_t acceptedSeeds = 0;
    uint32_t refinedSeeds = 0;
    uint32_t lowConfidenceSeeds = 0;
    float dominantMotionX = 0.0f;
    float dominantMotionY = 0.0f;
    float meanResidual = 0.0f;
    float meanConfidence = 0.0f;
    double cpuMilliseconds = 0.0;
    bool sceneCut = false;
};

class MotionEstimator {
public:
    // beginFrame resets per-frame counters. It is called on the decode thread
    // immediately before estimate(), so statistics describe one input pair.
    void beginFrame(bool sceneCut);

    // estimate converts already-normalized codec blocks into the same causal
    // source-pixel convention. Codec mode returns filtered seeds; refined mode
    // performs a bounded 3x3 luma search around each seed and scores residual.
    // No future vector is accepted here, and no output is fabricated when the
    // previous luma frame is unavailable.
    std::vector<MvEntry> estimate(const MotionEstimatorConfig& config,
                                  const LumaBuffer& current,
                                  const LumaBuffer& previous,
                                  const std::vector<MvEntry>& codecSeeds,
                                  uint32_t sourceWidth,
                                  uint32_t sourceHeight);

    [[nodiscard]] const MotionEstimatorStats& stats() const { return stats_; }

    // Aggregate sparse-field trust for the frame-level temporal policy.
    // Upstream: causal codec blocks after validation/refinement. Downstream:
    // SideBufferSynth scene-cut/jitter policy and FSR history confidence.
    // The area-weighted block confidence is part of the result; geometric
    // coverage and vector consistency alone are not sufficient evidence.
    static float aggregateConfidence(const std::vector<MvEntry>& mvs,
                                     int width, int height,
                                     float emptyConfidence = 0.5f,
                                     bool includeLocalConfidence = true);

    // A single parser for runtime controls. Invalid values fail closed to the
    // disabled mode; callers can use this without duplicating env parsing.
    static MotionEstimatorConfig configFromEnvironment();

private:
    MotionEstimatorStats stats_{};
};

} // namespace temporal_forge
