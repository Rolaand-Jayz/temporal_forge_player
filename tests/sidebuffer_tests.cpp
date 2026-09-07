// sidebuffer_tests.cpp — spec 03 side-buffer synthesis.
// Validates the reactive mask formula, scene-cut thresholds, and histogram
// delta computation.
#include "render/SideBufferSynth.hpp"

#include <cmath>
#include <cstdlib>
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
    // The configured histogram threshold must be honored by the runtime
    // overload; this keeps Quality Lab motion policy from being parsed but
    // ignored by the live scene-cut detector.
    CHECK(S::shouldReset(0.40f, 1.0f, 16.0f, 16.0f, 0.35f) == true);
    CHECK(S::shouldReset(0.40f, 1.0f, 16.0f, 16.0f, 0.45f) == false);
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

// A failed FSR submission must not advance the analysis frame paired with
// temporal history. The next motion estimate must still compare against the
// last successfully submitted frame, not the decoded frame that failed.
static void test_failed_frame_rolls_back_analysis_history() {
    LumaBuffer frameA;
    frameA.width = 8;
    frameA.height = 8;
    frameA.data.assign(64, 0.2f);
    LumaBuffer frameB = frameA;
    frameB.data.assign(64, 0.8f);

    SideBufferSynth synth;
    synth.update(frameA, 16.7f, false);
    const LumaBuffer previousBeforeFailure = synth.previousLuma();
    synth.update(frameB, 16.7f, false);
    // Simulate the complete FSR submission failing after side inputs were
    // prepared. rollbackJitter is the existing frame-abort boundary used by
    // PlaybackEngine, so it must restore all state advanced by update().
    synth.rollbackJitter(false);

    CHECK(synth.previousLuma().width == previousBeforeFailure.width);
    CHECK(synth.previousLuma().height == previousBeforeFailure.height);
    CHECK(synth.previousLuma().data == previousBeforeFailure.data);
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
        // This matcher compares the current decoded frame with the prior
        // displayed decoded frame directly, so its vector is a known causal
        // current-to-previous field and must survive the shared validation
        // boundary used by the temporal path.
        CHECK(mv.source == -1);
        CHECK_FEQ(mv.mvX, 0.0f);
        CHECK_FEQ(mv.mvY, 0.0f);
        CHECK(mv.dstX >= 0);
        CHECK(mv.dstY >= 0);
        // A perfect local match may be trusted, but the fallback must not
        // stamp every block with implicit full confidence.
        CHECK(mv.confidence > 0.99f);
    }

    LumaBuffer unrelated = frame;
    unrelated.data.assign(frame.data.size(), 0.95f);
    synth.update(unrelated, 16.7f, false);
    const auto mismatched = synth.estimateFallbackMotion(
        frame, frame.width, frame.height);
    bool sawLowConfidence = false;
    for (const auto& mv : mismatched)
        sawLowConfidence |= mv.confidence < 0.5f;
    CHECK(sawLowConfidence);
}

// A wider search must be available only as an explicit capture-time opt-in.
// Upstream: two analysis-luma frames with a six-pixel displacement. Downstream:
// the fallback motion field consumed by PlaybackEngine. The default radius
// remains four so ordinary playback keeps its established CPU cost and output.
static void test_fallback_motion_opt_in_radius() {
    LumaBuffer previous;
    previous.width = 32;
    previous.height = 16;
    previous.data.resize(previous.width * previous.height);
    for (uint32_t y = 0; y < previous.height; ++y) {
        for (uint32_t x = 0; x < previous.width; ++x) {
            // A non-repeating pattern makes the six-pixel correspondence
            // distinguishable from the clipped four-pixel boundary candidate.
            previous.data[y * previous.width + x] =
                static_cast<float>((x * 17u + y * 31u + x * y * 7u) % 251u) /
                250.0f;
        }
    }
    LumaBuffer current = previous;
    for (uint32_t y = 0; y < current.height; ++y) {
        for (uint32_t x = 0; x < current.width; ++x) {
            const uint32_t sourceX = x >= 6 ? x - 6 : 0;
            current.data[y * current.width + x] =
                previous.data[y * previous.width + sourceX];
        }
    }

    SideBufferSynth synth;
    synth.update(previous, 16.7f, false);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS");
    const auto defaultRadius = synth.estimateFallbackMotion(
        current, current.width, current.height);
    setenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS", "8", 1);
    const auto widerRadius = synth.estimateFallbackMotion(
        current, current.width, current.height);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS");

    const auto findBlock = [](const std::vector<MvEntry>& values) {
        for (const auto& value : values)
            if (value.dstX == 8 && value.dstY == 4) return value.mvX;
        return 999.0f;
    };
    CHECK(std::abs(findBlock(defaultRadius)) <= 4.01f);
    CHECK_FEQ(findBlock(widerRadius), -6.0f);
}

// The wider window also supports an opt-in coarse-to-fine search so capture
// experiments can test larger motion without paying for every displacement.
// Upstream: the same shifted luma pair as the radius test. Downstream: the
// motion field used by the temporal history path; absent the flag, no behavior
// or CPU cost changes.
static void test_fallback_motion_coarse_to_fine_opt_in() {
    LumaBuffer previous;
    previous.width = 32;
    previous.height = 16;
    previous.data.resize(previous.width * previous.height);
    for (uint32_t y = 0; y < previous.height; ++y)
        for (uint32_t x = 0; x < previous.width; ++x)
            previous.data[y * previous.width + x] =
                static_cast<float>((x * 19u + y * 23u + x * y * 11u) % 251u) /
                250.0f;
    LumaBuffer current = previous;
    for (uint32_t y = 0; y < current.height; ++y)
        for (uint32_t x = 0; x < current.width; ++x) {
            const uint32_t sourceX = x >= 7 ? x - 7 : 0;
            current.data[y * current.width + x] =
                previous.data[y * previous.width + sourceX];
        }

    SideBufferSynth synth;
    synth.update(previous, 16.7f, false);
    setenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_COARSE_TO_FINE", "1", 1);
    const auto result = synth.estimateFallbackMotion(
        current, current.width, current.height);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_COARSE_TO_FINE");

    bool foundExpected = false;
    for (const auto& value : result) {
        if (value.dstX == 8 && value.dstY == 4) {
            // The clipped left boundary can make the neighboring -8 sample
            // marginally cheaper for this synthetic fixture; the contract is
            // that coarse-to-fine can recover motion outside the old ±4 box.
            CHECK(std::abs(value.mvX) > 4.0f);
            foundExpected = true;
        }
    }
    CHECK(foundExpected);
}

// Larger diagnostic blocks are available for compressed material where 4x4
// luma patches are ambiguous. The default block geometry must remain intact.
static void test_fallback_motion_block_size_opt_in() {
    LumaBuffer frame;
    frame.width = 16;
    frame.height = 16;
    frame.data.resize(frame.width * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y)
        for (uint32_t x = 0; x < frame.width; ++x)
            frame.data[y * frame.width + x] =
                static_cast<float>((x * 13u + y * 29u) % 127u) / 126.0f;
    SideBufferSynth synth;
    synth.update(frame, 16.7f, false);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_BLOCK_SIZE");
    const auto defaultBlocks = synth.estimateFallbackMotion(frame, 160, 160);
    setenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_BLOCK_SIZE", "8", 1);
    const auto largerBlocks = synth.estimateFallbackMotion(frame, 160, 160);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_BLOCK_SIZE");
    CHECK(defaultBlocks.size() == 16);
    CHECK(largerBlocks.size() == 4);
}

// Bidirectional consistency should retain a past vector while using the
// next-frame estimate as a cheap cross-check. Only same-area vectors are
// fused, so unrelated blocks are not allowed to invent correspondence.
static void test_bidirectional_motion_consistency() {
    MvEntry past;
    past.dstX = 32; past.dstY = 16; past.w = 8; past.h = 8;
    past.mvX = -4.0f; past.mvY = 1.0f; past.confidence = 1.0f;
    MvEntry future = past;
    future.mvX = -6.0f; future.mvY = 1.0f;
    const auto fused = SideBufferSynth::fuseBidirectionalMotion(
        {past}, {future}, 0.0f);
    CHECK(fused.size() == 1);
    CHECK_FEQ(fused.front().mvX, -5.0f);
    CHECK(fused.front().confidence > 0.0f);
}

// Future evidence may reduce trust, but it must not rewrite the causal
// vector or turn the buffered frame into a displayed/interpolated frame.
static void test_future_evidence_only_preserves_causal_vector() {
    MvEntry past;
    past.dstX = 32; past.dstY = 16; past.w = 8; past.h = 8;
    past.mvX = -4.0f; past.mvY = 1.0f; past.confidence = 1.0f;
    // The same moving block is four source pixels farther right in the future
    // frame; its future->current vector maps it back to the causal location.
    MvEntry future = past;
    future.dstX = 36;
    future.mvX = -6.0f;
    const auto gated = SideBufferSynth::gateMotionWithFutureEvidence(
        {past}, {future}, 1.0f);
    CHECK(gated.size() == 1);
    CHECK_FEQ(gated.front().mvX, past.mvX);
    CHECK_FEQ(gated.front().mvY, past.mvY);
    CHECK(gated.front().confidence < past.confidence);
}

// Opt-in history-confidence primitive: retain a codec vector when its local
// previous-frame patch is plausible, and reject an obviously wrong vector so
// the GPU validity texture can fall back to current reconstruction.
static void test_codec_motion_validation_rejects_bad_seed() {
    LumaBuffer previous;
    previous.width = 32;
    previous.height = 16;
    previous.data.resize(previous.width * previous.height);
    for (uint32_t y = 0; y < previous.height; ++y) {
        for (uint32_t x = 0; x < previous.width; ++x)
            previous.data[y * previous.width + x] =
                static_cast<float>((x * 3u + y * 5u) % 31u) / 30.0f;
    }
    LumaBuffer current = previous;
    SideBufferSynth synth;
    synth.update(previous, 16.7f, false);

    MvEntry good;
    good.dstX = 8; good.dstY = 4; good.w = 8; good.h = 8;
    good.mvX = 0.0f; good.mvY = 0.0f; good.source = -1;
    MvEntry bad = good;
    bad.mvX = 9.0f;
    const auto validated = synth.validateCodecMotion(
        current, {good, bad}, previous.width, previous.height, 0.04f);
    CHECK(validated.size() == 1);
    CHECK(validated.front().mvX == 0.0f);
}

static void test_codec_motion_score_is_continuous() {
    LumaBuffer frame;
    frame.width = 16;
    frame.height = 16;
    frame.data.resize(frame.width * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y)
        for (uint32_t x = 0; x < frame.width; ++x)
            frame.data[y * frame.width + x] =
                static_cast<float>((x + y * 3u) % 17u) / 16.0f;
    SideBufferSynth synth;
    synth.update(frame, 16.7f, false);
    MvEntry good;
    good.dstX = 4; good.dstY = 4; good.w = 8; good.h = 8; good.source = -1;
    MvEntry weak = good;
    weak.mvX = 4.0f;
    const auto scored = synth.scoreCodecMotion(
        frame, {good, weak}, frame.width, frame.height, 0.04f);
    CHECK(scored.size() == 2);
    CHECK(scored[0].confidence > scored[1].confidence);
    CHECK(scored[0].confidence > 0.9f);
    CHECK(scored[1].confidence >= 0.0f && scored[1].confidence < 1.0f);
}

int main() {
    test_reactive_formula();
    test_scene_cut_thresholds();
    test_histogram_delta();
    test_static_vs_cut();
    test_pts_gap_uses_prior_cadence();
    test_first_interval_establishes_cadence();
    test_failed_frame_rolls_back_analysis_history();
    test_low_res_jitter_scaling();
    test_jitter_strength_override();
    test_jitter_sequence_and_cadence_controls();
    test_motion_confidence_can_raise_reactive_uncertainty();
    test_fallback_motion_has_causal_state();
    test_fallback_motion_opt_in_radius();
    test_fallback_motion_coarse_to_fine_opt_in();
    test_fallback_motion_block_size_opt_in();
    test_bidirectional_motion_consistency();
    test_future_evidence_only_preserves_causal_vector();
    test_codec_motion_validation_rejects_bad_seed();
    test_codec_motion_score_is_continuous();
    if (g_failures == 0) { std::printf("sidebuffer_tests: OK\n"); return 0; }
    std::fprintf(stderr, "sidebuffer_tests: %d FAILURES\n", g_failures);
    return 1;
}
