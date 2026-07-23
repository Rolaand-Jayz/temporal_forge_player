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

int main() {
    test_reactive_formula();
    test_scene_cut_thresholds();
    test_histogram_delta();
    test_static_vs_cut();
    test_low_res_jitter_scaling();
    if (g_failures == 0) { std::printf("sidebuffer_tests: OK\n"); return 0; }
    std::fprintf(stderr, "sidebuffer_tests: %d FAILURES\n", g_failures);
    return 1;
}
