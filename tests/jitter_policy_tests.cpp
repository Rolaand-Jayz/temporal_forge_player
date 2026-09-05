// jitter_policy_tests.cpp — M5 tests for decoded-video jitter policy.
//
// The policy is deliberately tested at SideBufferSynth's public boundary. It
// must make the diagnostic choice explicit, keep amplitude deterministic, and
// restart the Halton phase after a reset. These tests are written before the
// policy implementation and are not allowed to be weakened to fit it.
#include "render/SideBufferSynth.hpp"

#include <cmath>
#include <cstdio>

using temporal_forge::JitterMode;
using temporal_forge::LumaBuffer;
using temporal_forge::SideBufferInputs;
using temporal_forge::SideBufferSynth;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

static LumaBuffer stableLuma() {
    LumaBuffer luma;
    luma.width = 2;
    luma.height = 2;
    luma.data = {0.25f, 0.25f, 0.25f, 0.25f};
    return luma;
}

static SideBufferInputs firstFrame(JitterMode mode, float controlled = 1.0f) {
    SideBufferSynth synth;
    synth.setRenderSize(1920, 1080);
    synth.setJitterMode(mode);
    synth.setControlledJitterStrength(controlled);
    return synth.update(stableLuma(), 16.6667f, true, 1.0f);
}

static SideBufferInputs firstLowResolutionFrame(bool fullAmplitude) {
    SideBufferSynth synth;
    synth.setRenderSize(640, 360);
    synth.setJitterMode(JitterMode::Current);
    synth.setFullAmplitudeJitter(fullAmplitude);
    return synth.update(stableLuma(), 16.6667f, true, 1.0f);
}

static SideBufferInputs nextFrame(SideBufferSynth& synth) {
    return synth.update(stableLuma(), 16.6667f, false, 1.0f);
}

int main() {
    const auto off = firstFrame(JitterMode::Off);
    CHECK(near(off.jitterX, 0.0f));
    CHECK(near(off.jitterY, 0.0f));

    const auto current = firstFrame(JitterMode::Current);
    CHECK(std::fabs(current.jitterX) > 0.0f || std::fabs(current.jitterY) > 0.0f);

    const auto reduced = firstFrame(JitterMode::Reduced);
    CHECK(std::fabs(reduced.jitterX) <= std::fabs(current.jitterX) + 1e-6f);
    CHECK(std::fabs(reduced.jitterY) <= std::fabs(current.jitterY) + 1e-6f);

    const auto controlled = firstFrame(JitterMode::Controlled, 0.25f);
    CHECK(std::fabs(controlled.jitterX) <= std::fabs(current.jitterX) + 1e-6f);
    CHECK(std::fabs(controlled.jitterY) <= std::fabs(current.jitterY) + 1e-6f);

    // The diagnostic full-amplitude mode must bypass only the low-resolution
    // taper. It should preserve the same Halton sample while increasing its
    // 640x360 render-pixel displacement; the normal path remains tapered.
    const auto taperedLow = firstLowResolutionFrame(false);
    const auto fullLow = firstLowResolutionFrame(true);
    CHECK(std::fabs(fullLow.jitterX) >= std::fabs(taperedLow.jitterX) - 1e-6f);
    CHECK(std::fabs(fullLow.jitterY) >= std::fabs(taperedLow.jitterY) - 1e-6f);
    CHECK(std::fabs(fullLow.jitterX) > std::fabs(taperedLow.jitterX) + 1e-6f ||
          std::fabs(fullLow.jitterY) > std::fabs(taperedLow.jitterY) + 1e-6f);

    SideBufferSynth resetSynth;
    resetSynth.setRenderSize(1920, 1080);
    resetSynth.setJitterMode(JitterMode::Current);
    const auto first = resetSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    (void)resetSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    const auto afterReset = resetSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    CHECK(near(first.jitterX, afterReset.jitterX));
    CHECK(near(first.jitterY, afterReset.jitterY));

    // A detected/forced reset must restart the phase on the reset frame
    // itself. Emitting the previous phase first would pair a fresh history
    // state with the wrong color sample and leave the next frame shifted.
    SideBufferSynth resetFrameSynth;
    resetFrameSynth.setRenderSize(1920, 1080);
    resetFrameSynth.setJitterMode(JitterMode::Current);
    const auto resetFirst =
        resetFrameSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    (void)resetFrameSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    const auto resetFrame =
        resetFrameSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    CHECK(near(resetFirst.jitterX, resetFrame.jitterX));
    CHECK(near(resetFirst.jitterY, resetFrame.jitterY));

    // A phase is part of the submitted FSR frame contract. Simulate a frame
    // that was prepared but never submitted, then roll it back; the next
    // update must present the same phase instead of silently skipping it.
    SideBufferSynth failedDispatchSynth;
    failedDispatchSynth.setRenderSize(1920, 1080);
    failedDispatchSynth.setJitterMode(JitterMode::Current);
    (void)failedDispatchSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    const auto prepared =
        failedDispatchSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    failedDispatchSynth.rollbackJitter(false);
    const auto retried =
        failedDispatchSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    CHECK(near(prepared.jitterX, retried.jitterX));
    CHECK(near(prepared.jitterY, retried.jitterY));

    // A cadence greater than one holds the exact submitted phase for the
    // requested number of FSR calls, then advances once. This protects the
    // frame-index contract used by the uploader and temporal dispatch.
    SideBufferSynth cadenceSynth;
    cadenceSynth.setRenderSize(1920, 1080);
    cadenceSynth.setJitterMode(JitterMode::Current);
    cadenceSynth.setJitterCadence(2);
    const auto cadenceFirst = cadenceSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    const auto cadenceHeld = nextFrame(cadenceSynth);
    const auto cadenceAdvanced = nextFrame(cadenceSynth);
    CHECK(near(cadenceFirst.jitterX, cadenceHeld.jitterX));
    CHECK(near(cadenceFirst.jitterY, cadenceHeld.jitterY));
    CHECK(!near(cadenceFirst.jitterX, cadenceAdvanced.jitterX) ||
          !near(cadenceFirst.jitterY, cadenceAdvanced.jitterY));

    // Sequence selection is data, not a display-only label: zero must remain
    // zero and the alternating diagnostic sequence must change direction.
    SideBufferSynth sequenceSynth;
    sequenceSynth.setRenderSize(1920, 1080);
    sequenceSynth.setJitterMode(JitterMode::Current);
    sequenceSynth.setJitterSequence(temporal_forge::JitterSequence::Zero);
    const auto zero = sequenceSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    CHECK(near(zero.jitterX, 0.0f));
    CHECK(near(zero.jitterY, 0.0f));
    sequenceSynth.setJitterSequence(temporal_forge::JitterSequence::Alternating);
    const auto alternatingA = nextFrame(sequenceSynth);
    const auto alternatingB = nextFrame(sequenceSynth);
    CHECK(!near(alternatingA.jitterX, alternatingB.jitterX) ||
          !near(alternatingA.jitterY, alternatingB.jitterY));

    if (g_failures == 0) {
        std::printf("jitter_policy_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "jitter_policy_tests: %d FAILURES\n", g_failures);
    return 1;
}
