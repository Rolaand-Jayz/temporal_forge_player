// sidebuffer_tests.cpp — spec 03 side-buffer synthesis.
// Validates the reactive mask formula, scene-cut thresholds, and histogram
// delta computation.
#include "render/SideBufferSynth.hpp"

#include <cmath>
#include <cstdio>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)
#define CHECK_FEQ(a, b) do { \
    if (std::fabs((a) - (b)) > 1e-5f) { std::fprintf(stderr, "FAIL %s:%d: %g != %g\n", __FILE__, __LINE__, (double)(a), (double)(b)); ++g_failures; } \
} while (0)

// spec 03 section 5: reactive = clamp(luma*0.6 + motion*0.3 + edge*0.1, 0, 0.85)
static void test_reactive_formula() {
    // All-zero inputs -> zero.
    CHECK_FEQ(SideBufferSynth::reactiveValue(0, 0, 0, ReactiveSynthMode::CheapAuto), 0.0f);
    // Max inputs -> capped at 0.85 (CheapAuto) / 0.9 (Aggressive).
    CHECK_FEQ(SideBufferSynth::reactiveValue(1, 1, 1, ReactiveSynthMode::CheapAuto), 0.85f);
    CHECK_FEQ(SideBufferSynth::reactiveValue(1, 1, 1, ReactiveSynthMode::Aggressive), 0.9f);
    // Mid: luma=0.5, motion=0.2, edge=0.1 -> 0.5*0.6+0.2*0.3+0.1*0.1 = 0.37
    CHECK_FEQ(SideBufferSynth::reactiveValue(0.5f, 0.2f, 0.1f, ReactiveSynthMode::CheapAuto), 0.37f);
}

// spec 03 section 8: scene-cut detection thresholds.
static void test_scene_cut_thresholds() {
    using S = SideBufferSynth;
    // histogramDelta > 0.65 -> reset
    CHECK(S::shouldReset(0.70f, 1.0f, 16.0f, 16.0f) == true);
    CHECK(S::shouldReset(0.60f, 1.0f, 16.0f, 16.0f) == false);
    // motionConfidence < 0.15 -> reset
    CHECK(S::shouldReset(0.10f, 0.10f, 16.0f, 16.0f) == true);
    CHECK(S::shouldReset(0.10f, 0.20f, 16.0f, 16.0f) == false);
    // ptsGap > 2.5x interval -> reset
    CHECK(S::shouldReset(0.10f, 1.0f, 50.0f, 16.0f) == true);  // 50 > 40
    CHECK(S::shouldReset(0.10f, 1.0f, 35.0f, 16.0f) == false); // 35 < 40
}

static void test_histogram_delta() {
    // Identical histograms -> 0 delta.
    std::array<uint32_t, 64> a{};
    a[0] = 10; a[1] = 20; a[2] = 30; a[3] = 40;
    CHECK_FEQ(histogramDelta(a, a), 0.0f);
    // Disjoint histograms -> 1.0.
    std::array<uint32_t, 64> b{};
    std::array<uint32_t, 64> c{};
    c[3] = 5;
    CHECK_FEQ(histogramDelta(b, c), 1.0f);
}

// A static scene (identical luma) should NOT trigger a reset, and reactive
// average should be low. A hard cut should trigger reset.
static void test_static_vs_cut() {
    LumaBuffer frame;
    frame.width = 16; frame.height = 9;
    frame.data.assign(16 * 9, 0.4f); // uniform mid-gray

    SideBufferSynth synth;
    // First frame: establishes previous.
    auto s0 = synth.update(frame, 41.7f, false); // 24fps
    CHECK(s0.reset == true); // first frame always resets (no previous)

    // Second identical frame: no cut.
    auto s1 = synth.update(frame, 41.7f, false);
    CHECK(s1.reset == false);
    CHECK(s1.histogramDelta < 0.01f);
    CHECK(s1.reactiveAverage < 0.1f);

    // Hard cut: completely different luma.
    LumaBuffer frame2;
    frame2.width = 16; frame2.height = 9;
    frame2.data.assign(16 * 9, 0.9f);
    auto s2 = synth.update(frame2, 41.7f, false);
    CHECK(s2.reset == true); // histogram delta = 1.0 > 0.65
}

static void test_pts_gap_uses_prior_cadence() {
    LumaBuffer frame;
    frame.width = 8; frame.height = 8;
    frame.data.assign(64, 0.4f);

    SideBufferSynth synth;
    synth.update(frame, 41.7f, false); // establish a 24-fps cadence
    auto normal = synth.update(frame, 41.7f, false);
    CHECK(normal.reset == false);

    // A 5x gap must reset against the prior 41.7 ms cadence. The old
    // implementation compared the gap to itself and could never detect it.
    auto gap = synth.update(frame, 208.5f, false);
    CHECK(gap.reset == true);

    // The discontinuity must not poison the cadence estimate: the next
    // normal frame is still interpreted as a regular frame.
    auto recovered = synth.update(frame, 41.7f, false);
    CHECK(recovered.reset == false);
}

// The first decoded frame has no PTS delta. The first real interval must
// establish the clip cadence before the gap detector compares it with the
// built-in 60-fps fallback; otherwise a normal 24-fps second frame is
// incorrectly treated as a scene cut.
static void test_first_interval_establishes_cadence() {
    LumaBuffer frame;
    frame.width = 8;
    frame.height = 8;
    frame.data.assign(64, 0.4f);

    SideBufferSynth synth;
    const auto first = synth.update(frame, 0.0f, false);
    CHECK(first.reset == true);
    const auto second = synth.update(frame, 41.6667f, false);
    CHECK(second.reset == false);
    CHECK_FEQ(second.expectedFrameIntervalMs, 41.6667f);
}

static void test_low_res_jitter_scaling() {
    LumaBuffer frame;
    frame.width = 640;
    frame.height = 360;
    frame.data.assign(frame.width * frame.height, 0.5f);

    SideBufferSynth synth;
    synth.setRenderSize(frame.width, frame.height);
    auto low = synth.update(frame, 41.7f, false);

    SideBufferSynth synthHigh;
    frame.width = 1920;
    frame.height = 1080;
    frame.data.assign(frame.width * frame.height, 0.5f);
    synthHigh.setRenderSize(frame.width, frame.height);
    auto high = synthHigh.update(frame, 41.7f, false);

    CHECK(std::fabs(high.jitterX) >= std::fabs(low.jitterX));
    CHECK(std::fabs(high.jitterY) >= std::fabs(low.jitterY));
}

static void test_jitter_strength_override() {
    LumaBuffer frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.data.assign(16, 0.5f);

    SideBufferSynth synth;
    synth.setRenderSize(frame.width, frame.height);
    synth.setJitterStrength(0.0f);
    const auto disabled = synth.update(frame, 16.7f, false);
    CHECK_FEQ(disabled.jitterX, 0.0f);
    CHECK_FEQ(disabled.jitterY, 0.0f);

    synth.setJitterStrength(1.0f);
    const auto enabled = synth.update(frame, 16.7f, false);
    CHECK(std::fabs(enabled.jitterX) > 0.0f ||
          std::fabs(enabled.jitterY) > 0.0f);
}

static void test_jitter_sequence_and_cadence_controls() {
    LumaBuffer frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.data.assign(16, 0.5f);

    SideBufferSynth swapped;
    swapped.setRenderSize(frame.width, frame.height);
    swapped.setJitterSequence(JitterSequence::Halton32);
    const auto first = swapped.update(frame, 16.7f, false);
    CHECK_FEQ(first.jitterX, -1.0f / 6.0f);
    CHECK_FEQ(first.jitterY, 0.0f);

    SideBufferSynth held;
    held.setRenderSize(frame.width, frame.height);
    held.setJitterCadence(2);
    held.update(frame, 16.7f, false);
    const auto heldFirst = held.update(frame, 16.7f, false);
    const auto heldSecond = held.update(frame, 16.7f, false);
    CHECK_FEQ(heldFirst.jitterX, heldSecond.jitterX);
    CHECK_FEQ(heldFirst.jitterY, heldSecond.jitterY);
}

// Opt-in quality candidate: when motion correspondence is uncertain but not
// low enough to hard-reset, reactive handling should reduce history trust.
// The default path remains unchanged until the caller enables this policy.
static void test_motion_confidence_can_raise_reactive_uncertainty() {
    LumaBuffer frame;
    frame.width = 8;
    frame.height = 8;
    frame.data.assign(64, 0.4f);

    SideBufferSynth trusted;
    trusted.setMotionConfidenceReactive(true);
    trusted.update(frame, 41.7f, false, 1.0f);
    const auto trustedFrame = trusted.update(frame, 41.7f, false, 1.0f);

    SideBufferSynth uncertain;
    uncertain.setMotionConfidenceReactive(true);
    uncertain.update(frame, 41.7f, false, 1.0f);
    const auto uncertainFrame = uncertain.update(frame, 41.7f, false, 0.20f);

    CHECK(trustedFrame.reset == false);
    CHECK(uncertainFrame.reset == false);
    CHECK(uncertainFrame.reactiveAverage > trustedFrame.reactiveAverage);
}

// The optional block-motion fallback must remain empty before a previous
// analysis frame exists, then produce valid source-space blocks for a static
// pair. Static blocks are useful: they distinguish covered zero motion from
// uncovered pixels in the GPU validity image.
static void test_fallback_motion_has_causal_state() {
    LumaBuffer frame;
    frame.width = 16;
    frame.height = 8;
    frame.data.resize(frame.width * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x)
            frame.data[y * frame.width + x] =
                0.2f + 0.03f * static_cast<float>((x + y) % 7);
    }

    SideBufferSynth synth;
    const auto first = synth.estimateFallbackMotion(frame, 160, 80);
    CHECK(first.empty());
    synth.update(frame, 16.7f, false);
    const auto second = synth.estimateFallbackMotion(frame, 160, 80);
    CHECK(!second.empty());
    for (const auto& mv : second) {
        CHECK(mv.w > 0);
        CHECK(mv.h > 0);
        CHECK_FEQ(mv.mvX, 0.0f);
        CHECK_FEQ(mv.mvY, 0.0f);
        CHECK(mv.dstX >= 0);
        CHECK(mv.dstY >= 0);
    }
}

int main() {
    test_reactive_formula();
    test_scene_cut_thresholds();
    test_histogram_delta();
    test_static_vs_cut();
    test_pts_gap_uses_prior_cadence();
    test_first_interval_establishes_cadence();
    test_low_res_jitter_scaling();
    test_jitter_strength_override();
    test_jitter_sequence_and_cadence_controls();
    test_motion_confidence_can_raise_reactive_uncertainty();
    test_fallback_motion_has_causal_state();
    if (g_failures == 0) { std::printf("sidebuffer_tests: OK\n"); return 0; }
    std::fprintf(stderr, "sidebuffer_tests: %d FAILURES\n", g_failures);
    return 1;
}
