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

    // Resolution-aware jitter scaling for low-res sources.
    void setRenderSize(uint32_t width, uint32_t height) {
        renderWidth_ = width;
        renderHeight_ = height;
    }
    void setJitterStrength(float v) { jitterStrength_ = v; }
    void setJitterMode(JitterMode mode) { jitterMode_ = mode; }
    void setControlledJitterStrength(float v) {
        controlledJitterStrength_ = std::clamp(v, 0.0f, 1.5f);
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

    // spec 03 scene-cut rule:
    //   reset = histogramDelta > 0.65
    //        OR motionConfidence < 0.15
    //        OR ptsGap > 2.5 * expectedFrameInterval
    static bool shouldReset(float histogramDelta,
                            float motionConfidence,
                            float ptsGapMs,
                            float expectedFrameIntervalMs);

private:
    DepthSynthMode depthMode_ = DepthSynthMode::Flat;
    ReactiveSynthMode reactiveMode_ = ReactiveSynthMode::CheapAuto;

    LumaBuffer previousLuma_;
    float previousAvgLuma_ = -1.0f;
    std::array<uint32_t, 64> previousHist_{}; // 64-bin histogram
    bool previousFrameValid_ = false;
    float expectedFrameIntervalMs_ = 16.6667f;
    uint32_t jitterIndex_ = 1;           // Halton sequence index
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    float jitterStrength_ = 1.0f;
    JitterMode jitterMode_ = JitterMode::Current;
    float controlledJitterStrength_ = 1.0f;
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
