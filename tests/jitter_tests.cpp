// jitter_tests.cpp — Halton(2,3) sequence sanity.
#include "util/Jitter.hpp"
#include <cstdio>
#include <cmath>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

int main() {
    // Halton(2,1) == 0.5; Halton(3,1) == 1/3.
    CHECK(std::fabs(halton(1, 2) - 0.5)  < 1e-9);
    CHECK(std::fabs(halton(1, 3) - (1.0/3.0)) < 1e-9);
    CHECK(std::fabs(halton(2, 2) - 0.25) < 1e-9);
    CHECK(std::fabs(halton(2, 3) - (2.0/3.0)) < 1e-9);

    // Jitter offsets must lie in [-0.5, 0.5).
    for (uint32_t i = 1; i <= 64; ++i) {
        const JitterOffset j = haltonJitter(i);
        CHECK(j.x >= -0.5f && j.x < 0.5f);
        CHECK(j.y >= -0.5f && j.y < 0.5f);
    }

    // Phase count: monotonic in render size, >= 1.
    CHECK(jitterPhaseCount(1,1)    >= 1u);
    CHECK(jitterPhaseCount(1920,1080) >= jitterPhaseCount(640,480));
    CHECK(jitterPhaseCount(3840,2160) >= jitterPhaseCount(1920,1080));

    // Jitter amplitude tapers for lower resolutions.
    CHECK(std::fabs(jitterAmplitudeScale(1920, 1080) - 1.0f) < 1e-6f);
    CHECK(jitterAmplitudeScale(640, 360) < jitterAmplitudeScale(1280, 720));
    CHECK(jitterAmplitudeScale(320, 240) <= 0.35f + 1e-6f);

    if (g_failures == 0) { std::printf("jitter_tests: OK\n"); return 0; }
    std::fprintf(stderr, "jitter_tests: %d FAILURES\n", g_failures);
    return 1;
}
