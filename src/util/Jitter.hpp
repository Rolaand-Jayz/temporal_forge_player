// Jitter.hpp — Halton(2,3) jitter sequence, FSR-equivalent.
// spec 03 section 2: "Use FSR query/helper functions where available, or
// implement Halton 2,3 equivalent." This is the standalone equivalent.
#pragma once
#include <cstdint>

namespace temporal_forge {

// halton: returns the i-th sample (1-indexed) of the Halton quasi-random
//         sequence for the given base.
//
// Called by: haltonJitter (this file).
// Notes:     i must be >= 1. Deterministic and constexpr; the same index/base
//            always yields the same value, which is what makes the jitter
//            sequence reproducible across frames.
constexpr double halton(uint32_t i, uint32_t base) {
    double f = 1.0;
    double r = 0.0;
    uint32_t idx = i;
    while (idx > 0) {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(idx % base);
        idx /= base;
    }
    return r;
}

struct JitterOffset {
    float x;
    float y;
};

// haltonJitter: Halton(2,3) sample i (1-indexed), mapped to [-0.5, 0.5).
//
// Called by: SideBufferSynth::update() each frame to offset the jittered color
//            input's texture coordinates.
// Calls:     halton.
// Notes:     FSR jitter convention — offset is in render-pixel units applied to
//            the jittered color input's texture coordinates.
constexpr JitterOffset haltonJitter(uint32_t i) {
    return JitterOffset{
        static_cast<float>(halton(i, 2) - 0.5),
        static_cast<float>(halton(i, 3) - 0.5),
    };
}

// jitterPhaseCount: FSR-equivalent jitter phase count for the render size.
//
// Called by: the FSR4 dispatch setup that decides how many jitter samples to
//            cycle through before repeating (keeps the sequence from visibly
//            tiling). Returns ~floor(log2(max(renderW, renderH))) + 1, the
//            standard FSR2 phase count over the longer render dimension.
// Notes:     constexpr.
constexpr uint32_t jitterPhaseCount(uint32_t renderWidth, uint32_t renderHeight) {
    uint32_t d = renderWidth > renderHeight ? renderWidth : renderHeight;
    uint32_t c = 1;
    while (d > 1) { d >>= 1; ++c; }
    return c;
}

// jitterAmplitudeScale: tapers jitter amplitude for small render sizes.
//
// Called by: SideBufferSynth::update(), multiplied by jitterStrength_ (the
//            user's [0.2, 1.5] setting from the QML tuning controls) to form
//            the per-frame jitter scale.
// Notes:     Lower-resolution sources are more sensitive to visible feature
//            blending from large subpixel motion, so amplitude tapers down as
//            the render size shrinks: 1.0 at >=1080p, 0.20 at <=360p, linear
//            in between. Keeps the sequence intact while reducing spatial spread.
constexpr float jitterAmplitudeScale(uint32_t renderWidth, uint32_t renderHeight) {
    const uint32_t d = renderWidth > renderHeight ? renderWidth : renderHeight;
    if (d >= 1080u) return 1.0f;
    if (d <= 360u) return 0.20f;
    const float t = static_cast<float>(d - 360u) / static_cast<float>(1080u - 360u);
    return 0.20f + 0.80f * t;
}

} // namespace temporal_forge
