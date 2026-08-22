// QualityLabConfig.cpp — validated runtime quality-lab JSON loading.
#include "config/QualityLabConfig.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace temporal_forge {

namespace {

QJsonObject childObject(const QJsonObject &parent, const char *key) {
    const QJsonValue value = parent.value(QLatin1String(key));
    return value.isObject() ? value.toObject() : QJsonObject{};
}

QJsonValue childValue(const QJsonObject &parent, const char *key) {
    return parent.value(QLatin1String(key));
}

float finiteClamped(const QJsonObject &object, const char *key, float fallback,
                   float minimum, float maximum) {
    const QJsonValue value = childValue(object, key);
    if (!value.isDouble())
        return fallback;
    const double parsed = value.toDouble();
    if (!std::isfinite(parsed))
        return fallback;
    return std::clamp(static_cast<float>(parsed), minimum, maximum);
}

bool readBool(const QJsonObject &object, const char *key, bool fallback) {
    const QJsonValue value = childValue(object, key);
    return value.isBool() ? value.toBool() : fallback;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string readName(const QJsonObject &object, const char *key,
                     std::string fallback) {
    const QJsonValue value = childValue(object, key);
    if (!value.isString())
        return fallback;
    return lower(value.toString().toStdString());
}

QualityCompositionMode parseComposition(const std::string &name) {
    if (name == "base_only" || name == "baseonly")
        return QualityCompositionMode::BaseOnly;
    if (name == "learned_only" || name == "learnedonly")
        return QualityCompositionMode::LearnedOnly;
    if (name == "direct_blend" || name == "directblend")
        return QualityCompositionMode::DirectBlend;
    if (name == "detail_residual" || name == "detailresidual")
        return QualityCompositionMode::DetailResidual;
    return QualityCompositionMode::Current;
}

QualityBaseFilterMode parseBaseFilter(const std::string &name) {
    if (name == "bilinear")
        return QualityBaseFilterMode::Bilinear;
    if (name == "mitchell" || name == "mitchell_netravali")
        return QualityBaseFilterMode::Mitchell;
    if (name == "lanczos2" || name == "lanczos")
        return QualityBaseFilterMode::Lanczos2;
    return QualityBaseFilterMode::CatmullRom;
}

QualityResidualLowpassMode parseResidualLowpass(const std::string &name) {
    return name == "gaussian3x3" || name == "gaussian"
               ? QualityResidualLowpassMode::Gaussian3x3
               : QualityResidualLowpassMode::Box3x3;
}

QualitySharpenMode parseSharpen(const std::string &name) {
    return name == "adaptive" ? QualitySharpenMode::Adaptive
                               : QualitySharpenMode::None;
}

QualityPresentationFilter parsePresentation(const std::string &name) {
    if (name == "nearest")
        return QualityPresentationFilter::Nearest;
    if (name == "linear" || name == "bilinear")
        return QualityPresentationFilter::Linear;
    if (name == "lanczos" || name == "lanczos2")
        return QualityPresentationFilter::Lanczos;
    return QualityPresentationFilter::Bicubic;
}

} // namespace

std::filesystem::path qualityLabConfigPath() {
    if (const char *overridePath = std::getenv("TFORGE_QUALITY_LAB_CONFIG")) {
        if (*overridePath)
            return std::filesystem::path(overridePath);
    }

    const auto relative = std::filesystem::current_path() / "config" /
                          "quality_lab.json";
    if (std::filesystem::exists(relative))
        return relative;

#ifdef TFORGE_SOURCE_ROOT
    const auto sourceRoot = std::filesystem::path(TFORGE_SOURCE_ROOT) /
                            "config" / "quality_lab.json";
    if (std::filesystem::exists(sourceRoot))
        return sourceRoot;
#endif

    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg)
            return std::filesystem::path(xdg) / "temporal-forge-player" /
                   "quality_lab.json";
    }
    if (const char *home = std::getenv("HOME")) {
        if (*home)
            return std::filesystem::path(home) / ".config" /
                   "temporal-forge-player" / "quality_lab.json";
    }
    return relative;
}

QualityLabConfig loadQualityLabConfig(const std::filesystem::path &path) {
    QualityLabConfig result;
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly))
        return result;

    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return result;

    const QJsonObject root = document.object();
    const QJsonObject lab = root.value(QLatin1String("qualityLab")).isObject()
                                ? root.value(QLatin1String("qualityLab")).toObject()
                                : root;
    result.enabled = readBool(lab, "enabled", result.enabled);

    const QJsonObject composition = childObject(lab, "composition");
    result.compositionMode = parseComposition(
        readName(composition, "mode", "current"));
    result.learnedStrength = finiteClamped(
        composition, "learnedStrength", result.learnedStrength, 0.0f, 1.0f);
    result.residualStrength = finiteClamped(
        composition, "residualStrength", result.residualStrength, 0.0f, 2.0f);

    const QJsonObject baseFilter = childObject(lab, "baseFilter");
    result.baseFilterMode = parseBaseFilter(
        readName(baseFilter, "mode", "catmull_rom"));
    result.baseB = finiteClamped(baseFilter, "b", result.baseB, -1.0f, 1.0f);
    result.baseC = finiteClamped(baseFilter, "c", result.baseC, -1.0f, 1.0f);

    const QJsonObject residual = childObject(lab, "residual");
    result.residualLowpassMode = parseResidualLowpass(
        readName(residual, "lowpassMode", "box3x3"));
    result.residualRadius = finiteClamped(
        residual, "radius", result.residualRadius, 0.25f, 2.0f);
    result.residualSigma = finiteClamped(
        residual, "sigma", result.residualSigma, 0.1f, 4.0f);

    const QJsonObject sharpen = childObject(lab, "sharpen");
    result.sharpenMode = parseSharpen(readName(sharpen, "mode", "none"));
    result.sharpenStrength = finiteClamped(
        sharpen, "strength", result.sharpenStrength, 0.0f, 1.0f);
    result.sharpenLimit = finiteClamped(
        sharpen, "limit", result.sharpenLimit, 0.0f, 1.0f);
    result.sharpenThreshold = finiteClamped(
        sharpen, "threshold", result.sharpenThreshold, 0.0f, 1.0f);

    const QJsonObject tone = childObject(lab, "tone");
    result.toneExposureEV = finiteClamped(
        tone, "exposureEV", result.toneExposureEV, -4.0f, 4.0f);
    result.toneContrast = finiteClamped(
        tone, "contrast", result.toneContrast, -1.0f, 1.0f);
    result.toneContrastPivot = finiteClamped(
        tone, "contrastPivot", result.toneContrastPivot, 0.0f, 1.0f);
    result.toneGamma = finiteClamped(
        tone, "gamma", result.toneGamma, 0.1f, 3.0f);

    const QJsonObject presentation = childObject(lab, "presentation");
    result.presentationFilter = parsePresentation(
        readName(presentation, "filter", "bicubic"));
    return result;
}

std::string qualityCompositionModeName(QualityCompositionMode mode) {
    switch (mode) {
    case QualityCompositionMode::Current: return "current";
    case QualityCompositionMode::BaseOnly: return "base_only";
    case QualityCompositionMode::LearnedOnly: return "learned_only";
    case QualityCompositionMode::DirectBlend: return "direct_blend";
    case QualityCompositionMode::DetailResidual: return "detail_residual";
    }
    return "current";
}

std::string qualityBaseFilterModeName(QualityBaseFilterMode mode) {
    switch (mode) {
    case QualityBaseFilterMode::Bilinear: return "bilinear";
    case QualityBaseFilterMode::Mitchell: return "mitchell";
    case QualityBaseFilterMode::CatmullRom: return "catmull_rom";
    case QualityBaseFilterMode::Lanczos2: return "lanczos2";
    }
    return "catmull_rom";
}

std::string qualityResidualLowpassModeName(QualityResidualLowpassMode mode) {
    switch (mode) {
    case QualityResidualLowpassMode::Box3x3: return "box3x3";
    case QualityResidualLowpassMode::Gaussian3x3: return "gaussian3x3";
    }
    return "box3x3";
}

std::string qualitySharpenModeName(QualitySharpenMode mode) {
    return mode == QualitySharpenMode::Adaptive ? "adaptive" : "none";
}

std::string qualityPresentationFilterName(QualityPresentationFilter filter) {
    switch (filter) {
    case QualityPresentationFilter::Nearest: return "nearest";
    case QualityPresentationFilter::Linear: return "linear";
    case QualityPresentationFilter::Bicubic: return "bicubic";
    case QualityPresentationFilter::Lanczos: return "lanczos";
    }
    return "bicubic";
}

} // namespace temporal_forge
