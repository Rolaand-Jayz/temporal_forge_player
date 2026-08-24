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

    SideBufferSynth resetSynth;
    resetSynth.setRenderSize(1920, 1080);
    resetSynth.setJitterMode(JitterMode::Current);
    const auto first = resetSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    (void)resetSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    (void)resetSynth.update(stableLuma(), 16.6667f, true, 1.0f);
    const auto afterReset = resetSynth.update(stableLuma(), 16.6667f, false, 1.0f);
    CHECK(near(first.jitterX, afterReset.jitterX));
    CHECK(near(first.jitterY, afterReset.jitterY));

    if (g_failures == 0) {
        std::printf("jitter_policy_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "jitter_policy_tests: %d FAILURES\n", g_failures);
    return 1;
}
