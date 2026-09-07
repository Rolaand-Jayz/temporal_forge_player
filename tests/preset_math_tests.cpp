// preset_math_tests.cpp — spec 02 & 07 worked examples.
// Validates the exact target sizes the spec mandates.
#include "util/FsrTargetMath.hpp"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <initializer_list>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static void test_preset_ratios() {
    CHECK(presetRatio(UpscalePreset::NativeAA)         == 1.0f);
    CHECK(presetRatio(UpscalePreset::Quality)          == 1.5f);
    CHECK(presetRatio(UpscalePreset::Balanced)         == 1.7f);
    CHECK(presetRatio(UpscalePreset::Performance)      == 2.0f);
    CHECK(presetRatio(UpscalePreset::UltraPerformance) == 3.0f);
}

// spec 07: "Scaling Tests / Preset Math" — exact expected values.
static void test_1080p_targets() {
    // 1920x1080
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Quality,2).width  == 2880);
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Quality,2).height == 1620);

    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Balanced,2).width  == 3264);
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Balanced,2).height == 1836);

    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Performance,2).width  == 3840);
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::Performance,2).height == 2160);

    CHECK(fsrTargetSize(1920,1080, UpscalePreset::UltraPerformance,2).width  == 5760);
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::UltraPerformance,2).height == 3240);

    CHECK(fsrTargetSize(1920,1080, UpscalePreset::NativeAA,2).width  == 1920);
    CHECK(fsrTargetSize(1920,1080, UpscalePreset::NativeAA,2).height == 1080);
}

static void test_720p_targets() {
    // 1280x720
    CHECK(fsrTargetSize(1280,720, UpscalePreset::Quality,2).width  == 1920);
    CHECK(fsrTargetSize(1280,720, UpscalePreset::Quality,2).height == 1080);

    CHECK(fsrTargetSize(1280,720, UpscalePreset::Balanced,2).width  == 2176);
    CHECK(fsrTargetSize(1280,720, UpscalePreset::Balanced,2).height == 1224);

    CHECK(fsrTargetSize(1280,720, UpscalePreset::Performance,2).width  == 2560);
    CHECK(fsrTargetSize(1280,720, UpscalePreset::Performance,2).height == 1440);

    CHECK(fsrTargetSize(1280,720, UpscalePreset::UltraPerformance,2).width  == 3840);
    CHECK(fsrTargetSize(1280,720, UpscalePreset::UltraPerformance,2).height == 2160);
}

// Even alignment must always hold (spec 02: minimum 2px).
static void test_even_alignment() {
    for (UpscalePreset p : {
        UpscalePreset::Quality, UpscalePreset::Balanced,
        UpscalePreset::Performance, UpscalePreset::UltraPerformance }) {
        Size2D t = fsrTargetSize(853, 480, p, 2); // odd source width
        CHECK((t.width & 1u) == 0u);
        CHECK((t.height & 1u) == 0u);
    }
}

// 8px alignment (spec 02 preferred).
static void test_8px_alignment() {
    Size2D t = fsrTargetSize(1920,1080, UpscalePreset::Balanced, 8);
    CHECK((t.width  % 8u) == 0u);
    CHECK((t.height % 8u) == 0u);
}

// spec 02 auto-match display examples.
static void test_auto_match() {
    CHECK(autoMatchPreset(1920,1080, 3840,2160) == UpscalePreset::Performance);
    CHECK(autoMatchPreset(1280,720,  2560,1440) == UpscalePreset::Performance);
    CHECK(autoMatchPreset(1280,720,  3840,2160) == UpscalePreset::UltraPerformance);
}

static void test_native_int8_ultraperformance_targets() {
    CHECK(nativeInt8UltraPerformanceTarget(426, 240).width == 1920);
    CHECK(nativeInt8UltraPerformanceTarget(640, 360).height == 1080);
    CHECK(nativeInt8UltraPerformanceTarget(854, 480).width == 3840);
    CHECK(nativeInt8UltraPerformanceTarget(1280, 720).height == 2160);
    CHECK(nativeInt8UltraPerformanceTarget(320, 240).width == 0);
    CHECK(nativeInt8UltraPerformanceTarget(1920, 1080).width == 3840);
    CHECK(nativeInt8UltraPerformanceTarget(2560, 1440).width == 0);
}

static void test_arbitrary_aspect_targets_stay_native() {
    const Size2D quality =
        fsrTargetSize(640, 480, UpscalePreset::Quality, 2);
    CHECK(quality.width == 960 && quality.height == 720);
    CHECK(nativeInt8UltraPerformanceTarget(640, 480).width == 0);
    CHECK(nativeInt8FourThreeTarget(640, 480).width == 1440);
    CHECK(nativeInt8FourThreeTarget(640, 480).height == 1080);
    CHECK(nativeInt8FixedTarget(640, 480).width == 1440);
    CHECK(nativeInt8FourThreeTarget(1280, 960).width == 2880);
    CHECK(nativeInt8FourThreeTarget(1280, 960).height == 2160);
    CHECK(nativeInt8FixedTarget(1280, 960).width == 2880);

    const Size2D performance =
        fsrTargetSize(1280, 960, UpscalePreset::Performance, 2);
    CHECK(performance.width == 2560 && performance.height == 1920);
    CHECK(nativeInt8UltraPerformanceTarget(1280, 960).height == 0);
    CHECK(nativeInt8SourceAspectSupported(640, 480));
    CHECK(nativeInt8SourceAspectSupported(1280, 720));
}

static void test_progressive_chain() {
    const auto passes = fsrProgressivePassSizes(240, 135, 1920, 1080);
    CHECK(passes.size() == 6);
    CHECK(passes.front().width == 360 && passes.front().height == 204);
    CHECK(passes.back().width == 1920 && passes.back().height == 1080);
    for (size_t i = 1; i < passes.size(); ++i) {
        CHECK(passes[i].width >= passes[i - 1].width);
        CHECK(passes[i].height >= passes[i - 1].height);
        CHECK((passes[i].width & 1u) == 0u);
        CHECK((passes[i].height & 1u) == 0u);
    }
}

int main() {
    test_preset_ratios();
    test_1080p_targets();
    test_720p_targets();
    test_even_alignment();
    test_8px_alignment();
    test_auto_match();
    test_native_int8_ultraperformance_targets();
    test_arbitrary_aspect_targets_stay_native();
    test_progressive_chain();
    if (g_failures == 0) { std::printf("preset_math_tests: OK\n"); return 0; }
    std::fprintf(stderr, "preset_math_tests: %d FAILURES\n", g_failures);
    return 1;
}
