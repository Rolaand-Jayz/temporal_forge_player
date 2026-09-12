// quality_lab_config_tests.cpp — typed runtime Quality Lab parsing.
#include "config/QualityLabConfig.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static std::filesystem::path uniqueTempPath(const char *suffix) {
    return std::filesystem::temp_directory_path() /
           ("temporal_forge_quality_lab_test-" +
            std::to_string(::getpid()) + "-" + suffix);
}

// Scoped environment-variable guard: restores (or clears) the variable on
// destruction so path-resolution tests cannot leak state into each other.
class EnvGuard {
public:
    EnvGuard(const char *var, std::string value) : var_(var) {
        if (const char *old = std::getenv(var)) had_ = true, old_ = old;
        ::setenv(var, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (had_) ::setenv(var_.c_str(), old_.c_str(), 1);
        else ::unsetenv(var_.c_str());
    }
private:
    std::string var_;
    bool had_ = false;
    std::string old_;
};

// Scoped working-directory change with restore.
class CwdGuard {
public:
    explicit CwdGuard(std::filesystem::path next) : old_(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }
    ~CwdGuard() { std::error_code ec; std::filesystem::current_path(old_, ec); }
private:
    std::filesystem::path old_;
};

static void test_missing_file_is_disabled_control() {
    const auto config = loadQualityLabConfig(uniqueTempPath("missing.json"));
    CHECK(!config.enabled);
    CHECK(config.compositionMode == QualityCompositionMode::Current);
    CHECK(config.baseFilterMode == QualityBaseFilterMode::CatmullRom);
    CHECK(std::fabs(config.toneGamma - 1.0f) < 1e-6f);
}

static void test_nested_values_and_clamps() {
    const auto path = uniqueTempPath("nested.json");
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

static void test_motion_values_are_resolved_from_quality_lab() {
    const auto path = uniqueTempPath("motion.json");
    {
        std::ofstream file(path);
        file << R"json({
          "qualityLab": {
            "enabled": true,
            "motion": {
              "mode": "codec_refined",
              "refinementScale": 8,
              "searchRadius": 3,
              "maxCorrectionPixels": 2.5,
              "minErrorImprovement": 0.01,
              "minErrorMargin": 0.02,
              "maxRefinedSeeds": 777,
              "confidenceErrorScale": 0.08,
              "confidenceThreshold": 0.25,
              "sceneCutThreshold": 0.7,
              "edgeAwareUpscale": false,
              "allowFallbackAfterFiltering": true
            }
          }
        })json";
    }
    const auto config = loadQualityLabConfig(path);
    CHECK(config.motionConfigured);
    CHECK(config.motion.mode == MotionEstimatorMode::CodecRefined);
    CHECK(config.motion.refinementScale == 8u);
    CHECK(config.motion.searchRadius == 3);
    CHECK(std::fabs(config.motion.maxCorrectionPixels - 2.5f) < 1e-6f);
    CHECK(std::fabs(config.motion.minErrorImprovement - 0.01f) < 1e-6f);
    CHECK(std::fabs(config.motion.minErrorMargin - 0.02f) < 1e-6f);
    CHECK(config.motion.maxRefinedSeeds == 777u);
    CHECK(std::fabs(config.motion.confidenceErrorScale - 0.08f) < 1e-6f);
    CHECK(std::fabs(config.motion.confidenceThreshold - 0.25f) < 1e-6f);
    CHECK(std::fabs(config.motion.sceneCutThreshold - 0.7f) < 1e-6f);
    CHECK(!config.motion.edgeAwareUpscale);
    CHECK(config.motion.allowFallbackAfterFiltering);
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

// --- qualityLabConfigPath() resolution-order coverage (F2) ---

static void test_path_env_override_wins_even_when_missing() {
    const auto overridePath = uniqueTempPath("env-override.json");
    // The env tier is unconditional: it wins over every file-based tier and
    // is returned even when the target file does not exist.
    std::error_code ec;
    std::filesystem::remove(overridePath, ec);
    EnvGuard env("TFORGE_QUALITY_LAB_CONFIG", overridePath.string());
    const auto resolved = qualityLabConfigPath();
    CHECK(resolved == overridePath);
    CHECK(!std::filesystem::exists(resolved));
}

static void test_path_cwd_tier_found_when_config_dir_present() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("tforge-qlab-path-cwd-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config");
    {
        std::ofstream file(root / "config" / "quality_lab.json");
        file << "{}";
    }
    {
        EnvGuard noEnv("TFORGE_QUALITY_LAB_CONFIG", "");
        ::unsetenv("TFORGE_QUALITY_LAB_CONFIG");
        CwdGuard cwd(root);
        const auto resolved = qualityLabConfigPath();
        CHECK(resolved == root / "config" / "quality_lab.json");
        CHECK(std::filesystem::exists(resolved));
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

static void test_path_xdg_tier_when_no_cwd_config_dir() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("tforge-qlab-path-xdg-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        EnvGuard noEnv("TFORGE_QUALITY_LAB_CONFIG", "");
        ::unsetenv("TFORGE_QUALITY_LAB_CONFIG");
        EnvGuard xdg("XDG_CONFIG_HOME", root.string());
        CwdGuard cwd(root); // root itself has no config/ subdir
        const auto resolved = qualityLabConfigPath();
        CHECK(resolved == root / "temporal-forge-player" / "quality_lab.json");
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

static void test_path_home_tier_when_xdg_unset() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("tforge-qlab-path-home-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        EnvGuard noEnv("TFORGE_QUALITY_LAB_CONFIG", "");
        ::unsetenv("TFORGE_QUALITY_LAB_CONFIG");
        EnvGuard noXdg("XDG_CONFIG_HOME", "");
        ::unsetenv("XDG_CONFIG_HOME");
        EnvGuard home("HOME", root.string());
        CwdGuard cwd(root);
        const auto resolved = qualityLabConfigPath();
        CHECK(resolved == root / ".config" / "temporal-forge-player" /
                              "quality_lab.json");
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

static void test_path_falls_back_to_cwd_relative_when_nothing_exists() {
    // When env override, exe-adjacent config, CWD config, XDG and HOME are
    // all absent, the function's final fallback is the CWD-relative
    // config/quality_lab.json path (never an empty path).
    const auto root = std::filesystem::temp_directory_path() /
                      ("tforge-qlab-path-none-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        EnvGuard noEnv("TFORGE_QUALITY_LAB_CONFIG", "");
        ::unsetenv("TFORGE_QUALITY_LAB_CONFIG");
        EnvGuard noXdg("XDG_CONFIG_HOME", "");
        ::unsetenv("XDG_CONFIG_HOME");
        EnvGuard noHome("HOME", "");
        ::unsetenv("HOME");
        CwdGuard cwd(root);
        const auto resolved = qualityLabConfigPath();
        CHECK(!resolved.empty());
        CHECK(resolved == root / "config" / "quality_lab.json");
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

int main() {
    test_missing_file_is_disabled_control();
    test_nested_values_and_clamps();
    test_motion_values_are_resolved_from_quality_lab();
    test_checked_in_quality_default_is_scale_aware_candidate();
    test_path_env_override_wins_even_when_missing();
    test_path_cwd_tier_found_when_config_dir_present();
    test_path_xdg_tier_when_no_cwd_config_dir();
    test_path_home_tier_when_xdg_unset();
    test_path_falls_back_to_cwd_relative_when_nothing_exists();
    if (g_failures == 0) {
        std::printf("quality_lab_config_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "quality_lab_config_tests: %d FAILURES\n", g_failures);
    return 1;
}
