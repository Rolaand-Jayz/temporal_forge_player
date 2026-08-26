// QualityLabConfig.hpp — runtime reconstruction-quality experiment controls.
//
// The quality campaign must be able to change image-formation parameters
// without rebuilding the neural graph. The loader intentionally owns one
// typed value object; render code receives resolved values instead of reading
// environment variables or parsing JSON on the dispatch path.
#pragma once

#include <filesystem>
#include <string>

namespace temporal_forge {

enum class QualityCompositionMode : unsigned char {
    Current,
    BaseOnly,
    LearnedOnly,
    DirectBlend,
    DetailResidual,
};

enum class QualityBaseFilterMode : unsigned char {
    Bilinear,
    Mitchell,
    CatmullRom,
    Lanczos2,
};

enum class QualityBaseColorSpace : unsigned char {
    Model,
    Display,
};

enum class QualityResidualLowpassMode : unsigned char {
    Box3x3,
    Gaussian3x3,
};

enum class QualitySharpenMode : unsigned char {
    None,
    Adaptive,
};

enum class QualityPresentationFilter : unsigned char {
    Nearest,
    Linear,
    Bicubic,
    Lanczos,
};

struct QualityLabConfig {
    bool enabled = false;

    QualityCompositionMode compositionMode = QualityCompositionMode::Current;
    float learnedStrength = 0.55f;
    float residualStrength = 1.0f;

    QualityBaseFilterMode baseFilterMode = QualityBaseFilterMode::CatmullRom;
    QualityBaseColorSpace baseColorSpace = QualityBaseColorSpace::Model;
    float baseB = 1.0f / 3.0f;
    float baseC = 1.0f / 3.0f;

    QualityResidualLowpassMode residualLowpassMode =
        QualityResidualLowpassMode::Box3x3;
    float residualRadius = 1.0f;
    float residualSigma = 0.85f;

    QualitySharpenMode sharpenMode = QualitySharpenMode::None;
    float sharpenStrength = 0.0f;
    float sharpenLimit = 0.25f;
    float sharpenThreshold = 0.05f;

    float toneExposureEV = 0.0f;
    float toneContrast = 0.0f;
    float toneContrastPivot = 0.5f;
    float toneGamma = 1.0f;

    QualityPresentationFilter presentationFilter =
        QualityPresentationFilter::Bicubic;
};

// Resolves the persistent lab file. TFORGE_QUALITY_LAB_CONFIG takes
// precedence, followed by config/quality_lab.json in the current checkout,
// the compiled source root, and the user's XDG config directory.
std::filesystem::path qualityLabConfigPath();

// Loads and validates one JSON file. Missing or malformed values retain the
// typed defaults. A missing file therefore leaves the current path disabled.
QualityLabConfig loadQualityLabConfig(const std::filesystem::path &path);

std::string qualityCompositionModeName(QualityCompositionMode mode);
std::string qualityBaseFilterModeName(QualityBaseFilterMode mode);
std::string qualityBaseColorSpaceName(QualityBaseColorSpace space);
std::string qualityResidualLowpassModeName(QualityResidualLowpassMode mode);
std::string qualitySharpenModeName(QualitySharpenMode mode);
std::string qualityPresentationFilterName(QualityPresentationFilter filter);

} // namespace temporal_forge
