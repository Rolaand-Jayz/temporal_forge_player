// SideBufferSynth.hpp — spec 03 "Cheap FSR Input Synthesis".
//
// Produces the synthetic FSR inputs video lacks:
//   - jitter offset (Halton 2,3)
//   - luma (current + previous, for scene-cut + reactive)
//   - cheap reactive mask
//   - flat / edge-lite compatibility depth for the shared descriptor contract
//   - cheap scene-cut detection
//
// Guiding principle (spec 03): "Cheap and stable beats expensive and
// theoretically perfect." No ML depth, no heavy optical flow, no CPU readback
// in the GPU path. These computations run on small downsampled analysis
// buffers for the decision logic; the actual mask textures are generated on
// GPU in the full path (Phase 3+).
#pragma once
#include "media/VideoDecoder.hpp"
#include "util/Jitter.hpp"
#include <algorithm>
#include <cstdint>
#include <array>
#include <vector>

namespace temporal_forge {

enum class DepthSynthMode { Flat, EdgeLite };
enum class ReactiveSynthMode { Off, CheapAuto, Aggressive };
// JitterMode makes decoded-video jitter an explicit diagnostic choice instead
// of an unnamed scalar. Current preserves playback behavior; the other modes
// are for controlled sequence comparisons and are recorded by the caller.
enum class JitterMode { Off, Current, Reduced, Controlled };
// JitterSequence selects an existing deterministic low-cost sample family.
// Halton(2,3) is the current default; the alternatives are opt-in capture
// probes and do not add temporal lookahead or optical flow.
enum class JitterSequence { Halton23, Halton32, Alternating, Zero };

// Cheap analysis buffer: a small luminance image used for scene-cut and
// reactive-mask heuristics. Quarter-resolution is plenty (spec 03 block
// matching uses quarter-res).
struct LumaBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> data; // [0,1] luma, row-major
};

struct SideBufferInputs {
    // Jitter
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // Scene-cut / reset decision
    bool reset = false;          // caller ORs this with seek/new-file resets
    float histogramDelta = 0.0f; // [0,1]
    float avgLumaDelta = 0.0f;   // [0,1]
    float motionConfidence = 1.0f;
    float expectedFrameIntervalMs = 0.0f;

    // Reactive mask statistics
    float reactiveAverage = 0.0f; // mean of the reactive map
};

class SideBufferSynth {
public:
    SideBufferSynth();

    // Call once per source frame (before backend dispatch).
    // lumaCurrent: downsampled luma of the current frame ([0,1], any size).
    // ptsDeltaMs:  frameTimeMs (current PTS - previous PTS).
    // forcedReset: seek/new-file/scene-cut from caller.
    SideBufferInputs update(const LumaBuffer& lumaCurrent,
                            float ptsDeltaMs,
                            bool forcedReset,
                            float motionConfidence = 1.0f);

    // Commit/rollback the jitter phase selected by the most recent update.
    // Upstream: update() prepares a phase before FSR submission. Downstream:
    // the next submitted frame must not skip a phase when dispatch fails.
    // resetPhase=true preserves a failed reset boundary while retrying.
    void commitJitter() { jitterRollbackValid_ = false; }
    void rollbackJitter(bool resetPhase);

    // Forget decoded-frame analysis state when a seek or file switch flushes
    // temporal history. Upstream: PlaybackEngine's decoder/FSR reset boundary.
    // Downstream: the next frame starts without stale luma, cadence, or jitter
    // state from the previous source sequence.
    void resetAnalysisHistory();

    // Estimate a sparse causal motion field from the previous and current
    // analysis-luma buffers. The returned vectors use source-pixel units so
    // PlaybackEngine can pass them through its existing source-to-model
    // scaling and GPU expansion path. This is deliberately an opt-in
    // fallback for clips whose decoder exports no codec motion vectors.
    std::vector<MvEntry> estimateFallbackMotion(const LumaBuffer& current,
                                                uint32_t sourceWidth,
                                                uint32_t sourceHeight) const;

    // Fuse causal motion with a next-frame estimate for an opt-in diagnostic.
    // Upstream: two sparse fields expressed in the same source-pixel space.
    // Downstream: the existing motion upload and temporal history sampler.
    // A zero consistency threshold uses the block-size-derived default.
    static std::vector<MvEntry> fuseBidirectionalMotion(
        const std::vector<MvEntry>& past,
        const std::vector<MvEntry>& future,
        float consistencyThreshold = 0.0f);

    // Use a buffered next frame only as evidence about whether a causal
    // vector is trustworthy. This preserves the causal vector components:
    // opposite-direction frame pairs must not be averaged into reprojection.
    // Upstream: current-to-previous and current-to-next analysis fields.
    // Downstream: existing validity/confidence upload and history sampling.
    static std::vector<MvEntry> gateMotionWithFutureEvidence(
        const std::vector<MvEntry>& past,
        const std::vector<MvEntry>& future,
        float consistencyThreshold = 0.0f);

    // Refine decoder-provided causal vectors with a tiny analysis-luma search.
    // Upstream: VideoDecoder's codec vectors seed the search. Downstream:
    // PlaybackEngine sends the corrected vectors through the existing model
    // scaling and GPU expansion path. This is deliberately local and bounded;
    // it is not a replacement for codec motion or a full optical-flow pass.
    std::vector<MvEntry> refineCodecMotion(const LumaBuffer& current,
                                           const std::vector<MvEntry>& seeds,
                                           uint32_t sourceWidth,
                                           uint32_t sourceHeight,
                                           int refinementRadius = 1,
                                           float maxCorrectionPixels = 1.0f) const;

    // Reject codec vectors whose destination patch cannot be explained by the
    // previous analysis frame. This is an opt-in confidence primitive: the
    // returned holes become invalid history in GpuImageUploader, while the
    // default codec path remains unchanged.
    std::vector<MvEntry> validateCodecMotion(const LumaBuffer& current,
                                             const std::vector<MvEntry>& seeds,
                                             uint32_t sourceWidth,
                                             uint32_t sourceHeight,
                                             float maxPatchError = 0.08f) const;

    // Annotate each causal vector with continuous local luma confidence while
    // preserving the vector set. The uploader carries this score to the
    // validity texture; the experimental prepass can then soften history
    // instead of making a binary keep/reject decision.
    std::vector<MvEntry> scoreCodecMotion(const LumaBuffer& current,
                                          const std::vector<MvEntry>& seeds,
                                          uint32_t sourceWidth,
                                          uint32_t sourceHeight,
                                          float errorScale = 0.04f) const;

    // Read-only previous analysis frame for the separate MotionEstimator.
    // Upstream: the previous call to update(); downstream: bounded luma
    // refinement only. No caller may mutate this buffer or use it as the FSR
    // color input.
    const LumaBuffer& previousLuma() const { return previousLuma_; }

    // Resolution-aware jitter scaling for low-res sources.
    void setRenderSize(uint32_t width, uint32_t height) {
        if (renderWidth_ != width || renderHeight_ != height) {
            jitterIndex_ = 1;
            jitterCadenceCounter_ = 0;
        }
        renderWidth_ = width;
        renderHeight_ = height;
        if (phaseRenderWidth_ == 0 || phaseRenderHeight_ == 0) {
            phaseRenderWidth_ = width;
            phaseRenderHeight_ = height;
        }
    }
    // Presentation width participates in the FSR phase-count query. Jitter is
    // still returned in render/source pixels; this setter only controls how
    // many Halton phases are used before repetition.
    void setPresentationSize(uint32_t width, uint32_t height) {
        if (presentationWidth_ != width || presentationHeight_ != height) {
            jitterIndex_ = 1;
            jitterCadenceCounter_ = 0;
        }
        presentationWidth_ = width;
        presentationHeight_ = height;
    }
    // Set the dimensions used only for the FSR phase-count query. The color
    // jitter amplitude remains based on decoded render dimensions above, but
    // phase repetition must follow the actual model/output pair, including
    // reduced or intermediate model paths.
    void setFsrJitterPair(uint32_t renderWidth, uint32_t renderHeight,
                          uint32_t presentationWidth,
                          uint32_t presentationHeight) {
        if (phaseRenderWidth_ != renderWidth ||
            phaseRenderHeight_ != renderHeight ||
            phasePresentationWidth_ != presentationWidth ||
            phasePresentationHeight_ != presentationHeight) {
            jitterIndex_ = 1;
            jitterCadenceCounter_ = 0;
        }
        phaseRenderWidth_ = renderWidth;
        phaseRenderHeight_ = renderHeight;
        phasePresentationWidth_ = presentationWidth;
        phasePresentationHeight_ = presentationHeight;
    }
    void setJitterStrength(float v) { jitterStrength_ = v; }
    void setJitterMode(JitterMode mode) { jitterMode_ = mode; }
    void setControlledJitterStrength(float v) {
        controlledJitterStrength_ = std::clamp(v, 0.0f, 1.5f);
    }
    void setJitterSequence(JitterSequence sequence) { jitterSequence_ = sequence; }
    // Quality-lab-only probe: use the authored Halton displacement in full
    // render-pixel units instead of the low-resolution safety taper. The
    // default remains false so ordinary video playback keeps its established
    // low-resolution jitter policy.
    void setFullAmplitudeJitter(bool enabled) { fullAmplitudeJitter_ = enabled; }
    void setJitterCadence(uint32_t cadence) {
        jitterCadence_ = std::clamp(cadence, 1u, 64u);
        // No setup frame is emitted by the playback loop. Start the counter
        // at zero so the selected phase is held for exactly `cadence` calls
        // to update(), including the first frame after a reset.
        jitterCadenceCounter_ = 0u;
    }

    // Reactive mask value per spec 03 section 5:
    //   reactive = clamp(lumaDiff*0.60 + motionUncertainty*0.30 + edgeChange*0.10, 0, cap)
    // Default cap 0.85; Aggressive mode raises cap to 0.9.
    static float reactiveValue(float lumaDifference,
                               float motionUncertainty,
                               float edgeChange,
                               ReactiveSynthMode mode);

    void setDepthMode(DepthSynthMode m) { depthMode_ = m; }
    void setReactiveMode(ReactiveSynthMode m) { reactiveMode_ = m; }

    // Opt-in temporal-quality probe: fold frame-level motion confidence into
    // the reactive uncertainty term without changing the default behavior.
    void setMotionConfidenceReactive(bool enabled) {
        motionConfidenceReactive_ = enabled;
    }

    // Configure the histogram threshold used by the live scene-cut detector.
    // The static four-argument overload remains the stable spec default for
    // callers and tests; PlaybackEngine uses this instance value for the
    // active Quality Lab configuration.
    void setSceneCutThreshold(float threshold) {
        sceneCutThreshold_ = std::clamp(threshold, 0.0f, 1.0f);
    }

    // spec 03 scene-cut rule:
    //   reset = histogramDelta > 0.65
    //        OR motionConfidence < 0.15
    //        OR ptsGap > 2.5 * expectedFrameInterval
    static bool shouldReset(float histogramDelta,
                            float motionConfidence,
                            float ptsGapMs,
                            float expectedFrameIntervalMs,
                            float histogramThreshold = 0.65f);

private:
    DepthSynthMode depthMode_ = DepthSynthMode::Flat;
    ReactiveSynthMode reactiveMode_ = ReactiveSynthMode::CheapAuto;

    LumaBuffer previousLuma_;
    float previousAvgLuma_ = -1.0f;
    std::array<uint32_t, 64> previousHist_{}; // 64-bin histogram
    bool previousFrameValid_ = false;
    // The first frame has no positive PTS interval. Keep cadence validity
    // separate from previous-frame validity so the first real interval can
    // establish a 24/30/60-fps baseline before gap detection runs.
    bool expectedIntervalEstablished_ = false;
    float expectedFrameIntervalMs_ = 16.6667f;
    uint32_t jitterIndex_ = 1;           // Halton sequence index
    uint32_t jitterCadenceCounter_ = 0;  // frames held at the current phase
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint32_t presentationWidth_ = 0;
    uint32_t presentationHeight_ = 0;
    uint32_t phaseRenderWidth_ = 0;
    uint32_t phaseRenderHeight_ = 0;
    uint32_t phasePresentationWidth_ = 0;
    uint32_t phasePresentationHeight_ = 0;
    float jitterStrength_ = 1.0f;
    JitterMode jitterMode_ = JitterMode::Current;
    float controlledJitterStrength_ = 1.0f;
    JitterSequence jitterSequence_ = JitterSequence::Halton23;
    uint32_t jitterCadence_ = 1;
    bool fullAmplitudeJitter_ = false;
    bool motionConfidenceReactive_ = false;
    float sceneCutThreshold_ = 0.65f;
    uint32_t jitterIndexBeforeUpdate_ = 1;
    uint32_t jitterCadenceCounterBeforeUpdate_ = 0;
    // Transaction snapshot for the frame currently being prepared. The
    // decode loop may advance analysis state before the GPU submission is
    // known to succeed; a failed submission must restore the last frame that
    // actually reached temporal history so the next MV pair stays aligned.
    LumaBuffer previousLumaBeforeUpdate_;
    float previousAvgLumaBeforeUpdate_ = -1.0f;
    std::array<uint32_t, 64> previousHistBeforeUpdate_{};
    bool previousFrameValidBeforeUpdate_ = false;
    bool expectedIntervalEstablishedBeforeUpdate_ = false;
    float expectedFrameIntervalBeforeUpdate_ = 16.6667f;
    bool jitterRollbackValid_ = false;
};

// Compute a 64-bin luminance histogram + average over a luma buffer.
struct LumaStats {
    std::array<uint32_t, 64> hist{}; // 64 bins
    float average = 0.0f;
};
LumaStats computeLumaStats(const LumaBuffer& luma);

// Histogram intersection-based delta in [0,1]. 0 = identical, 1 = disjoint.
float histogramDelta(const std::array<uint32_t, 64>& a,
                     const std::array<uint32_t, 64>& b);

} // namespace temporal_forge
