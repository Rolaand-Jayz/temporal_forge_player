// quality_lab_config_tests.cpp — typed runtime Quality Lab parsing.
#include "config/QualityLabConfig.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static void test_missing_file_is_disabled_control() {
    const auto config = loadQualityLabConfig(
        std::filesystem::temp_directory_path() /
        "temporal_forge_quality_lab_missing.json");
    CHECK(!config.enabled);
    CHECK(config.compositionMode == QualityCompositionMode::Current);
    CHECK(config.baseFilterMode == QualityBaseFilterMode::CatmullRom);
    CHECK(std::fabs(config.toneGamma - 1.0f) < 1e-6f);
}

static void test_nested_values_and_clamps() {
    const auto path = std::filesystem::temp_directory_path() /
                      "temporal_forge_quality_lab_test.json";
    {
        std::ofstream file(path);
        file << R"json({
          "qualityLab": {
            "enabled": true,
            "composition": {"mode":"detail_residual", "learnedStrength":3.0,
                              "residualStrength":-1.0},
            "baseFilter": {"mode":"mitchell", "b":0.25, "c":0.4},
            "residual": {"lowpassMode":"gaussian3x3", "radius":9.0,
                         "sigma":0.65},
            "sharpen": {"mode":"adaptive", "strength":0.3,
                        "limit":0.2, "threshold":0.07},
            "tone": {"exposureEV":-9.0, "contrast":0.1,
                     "contrastPivot":0.45, "gamma":1.1},
            "presentation": {"filter":"lanczos"}
          }
        })json";
    }
    const auto config = loadQualityLabConfig(path);
    CHECK(config.enabled);
    CHECK(config.compositionMode == QualityCompositionMode::DetailResidual);
    CHECK(std::fabs(config.learnedStrength - 1.0f) < 1e-6f);
    CHECK(std::fabs(config.residualStrength - 0.0f) < 1e-6f);
    CHECK(config.baseFilterMode == QualityBaseFilterMode::Mitchell);
    CHECK(config.baseColorSpace == QualityBaseColorSpace::Model);
    CHECK(std::fabs(config.baseB - 0.25f) < 1e-6f);
    CHECK(config.residualLowpassMode == QualityResidualLowpassMode::Gaussian3x3);
    CHECK(std::fabs(config.residualRadius - 2.0f) < 1e-6f);
    CHECK(config.sharpenMode == QualitySharpenMode::Adaptive);
    CHECK(std::fabs(config.toneExposureEV + 4.0f) < 1e-6f);
    CHECK(config.presentationFilter == QualityPresentationFilter::Lanczos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

static void test_checked_in_quality_default_is_scale_aware_candidate() {
    const auto config = loadQualityLabConfig(
        std::filesystem::path(TFORGE_SOURCE_ROOT) / "config" /
        "quality_lab.json");
    CHECK(config.enabled);
    CHECK(config.compositionMode == QualityCompositionMode::BaseOnly);
    CHECK(config.baseFilterMode == QualityBaseFilterMode::Bilinear);
    CHECK(config.baseColorSpace == QualityBaseColorSpace::Model);
    CHECK(config.sharpenMode == QualitySharpenMode::None);
    CHECK(std::fabs(config.toneExposureEV + 0.015f) < 1e-6f);
}

int main() {
    test_missing_file_is_disabled_control();
    test_nested_values_and_clamps();
    test_checked_in_quality_default_is_scale_aware_candidate();
    if (g_failures == 0) {
        std::printf("quality_lab_config_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "quality_lab_config_tests: %d FAILURES\n", g_failures);
    return 1;
}
