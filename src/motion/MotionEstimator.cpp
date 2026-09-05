// MotionEstimator.cpp — bounded codec-seeded luma refinement.
#include "motion/MotionEstimator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>

namespace temporal_forge {
namespace {

float sample(const LumaBuffer& image, int x, int y) {
    if (image.width == 0 || image.height == 0 || image.data.empty()) return 0.0f;
    x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
    return image.data[static_cast<size_t>(y) * image.width + x];
}

float sampleBilinear(const LumaBuffer& image, float x, float y) {
    if (image.width == 0 || image.height == 0 || image.data.empty())
        return 0.0f;
    x = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, static_cast<int>(image.width) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(image.height) - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float top = sample(image, x0, y0) * (1.0f - tx) +
                      sample(image, x1, y0) * tx;
    const float bottom = sample(image, x0, y1) * (1.0f - tx) +
                         sample(image, x1, y1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

float patchSadCachedCurrentFractional(
    const std::array<float, 9>& currentPatch, const LumaBuffer& previous,
    int x, int y, float dx, float dy) {
    // Half-pixel analysis candidates preserve sub-source-pixel motion after a
    // reduced-resolution search. Without bilinear sampling, a 2-source-pixel
    // move at a 1/4 grid becomes an incorrect 4-pixel quantized vector.
    float error = 0.0f;
    size_t index = 0;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox, ++index)
            error += std::abs(
                currentPatch[index] -
                sampleBilinear(previous, static_cast<float>(x + ox) + dx,
                                static_cast<float>(y + oy) + dy));
    return error / 9.0f;
}

float patchSadBilinear(const LumaBuffer& first, const LumaBuffer& second,
                       float x, float y, float dx, float dy) {
    // Use the same photometric metric in the reverse direction. This is a
    // cheap forward/backward check that rejects a one-way edge match which
    // looks good only because the search landed on a neighboring contour.
    float error = 0.0f;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
            error += std::abs(
                sampleBilinear(first, x + static_cast<float>(ox),
                               y + static_cast<float>(oy)) -
                sampleBilinear(second, x + dx + static_cast<float>(ox),
                               y + dy + static_cast<float>(oy)));
    return error / 9.0f;
}

float patchSad(const LumaBuffer& current, const LumaBuffer& previous,
               int x, int y, int dx, int dy) {
    float error = 0.0f;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
            error += std::abs(sample(current, x + ox, y + oy) -
                              sample(previous, x + dx + ox, y + dy + oy));
    return error / 9.0f;
}

float patchSadCachedCurrent(const std::array<float, 9>& currentPatch,
                            const LumaBuffer& previous, int x, int y,
                            int dx, int dy) {
    // The current 3x3 patch is invariant across all candidates for one seed.
    // Cache it once while preserving patch order and clamp-to-edge sampling;
    // the previous-frame side still changes with every candidate.
    float error = 0.0f;
    size_t index = 0;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox, ++index)
            error += std::abs(currentPatch[index] -
                              sample(previous, x + dx + ox, y + dy + oy));
    return error / 9.0f;
}

int parseInt(const char* value, int fallback, int minValue, int maxValue) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return fallback;
    return static_cast<int>(std::clamp(parsed, static_cast<long>(minValue),
                                       static_cast<long>(maxValue)));
}

float parseFloat(const char* value, float fallback, float minValue,
                 float maxValue) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(parsed)) return fallback;
    return std::clamp(parsed, minValue, maxValue);
}

// A spatially spread set of current-frame 3x3 patches. Global translation
// must be judged against many regions so one flat or repetitive patch cannot
// decide the frame's correspondence.
struct AnchorPatchSet {
    std::vector<std::pair<int, int>> centers;
    std::vector<std::array<float, 9>> currentPatches;
};

AnchorPatchSet buildAnchorPatches(const LumaBuffer& current,
                                  uint32_t patchCount) {
    AnchorPatchSet set;
    const uint32_t usableW = current.width > 4u ? current.width - 4u : 1u;
    const uint32_t usableH = current.height > 4u ? current.height - 4u : 1u;
    const uint32_t columns = std::max(
        1u, static_cast<uint32_t>(std::lround(
                std::sqrt(static_cast<double>(patchCount) *
                          static_cast<double>(usableW) /
                          static_cast<double>(std::max(1u, usableH))))));
    const uint32_t rows = std::max(1u, patchCount / columns);
    const float stepX = static_cast<float>(usableW) / static_cast<float>(columns);
    const float stepY = static_cast<float>(usableH) / static_cast<float>(rows);
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const int x = std::clamp(static_cast<int>(std::lround(
                (static_cast<float>(column) + 0.5f) * stepX)) + 2,
                0, static_cast<int>(current.width) - 1);
            const int y = std::clamp(static_cast<int>(std::lround(
                (static_cast<float>(row) + 0.5f) * stepY)) + 2,
                0, static_cast<int>(current.height) - 1);
            std::array<float, 9> patch{};
            size_t index = 0;
            for (int oy = -1; oy <= 1; ++oy)
                for (int ox = -1; ox <= 1; ++ox, ++index)
                    patch[index] = sample(current, x + ox, y + oy);
            set.centers.emplace_back(x, y);
            set.currentPatches.push_back(patch);
        }
    }
    return set;
}

float anchorSetSad(const AnchorPatchSet& set, const LumaBuffer& previous,
                   float dx, float dy) {
    if (set.centers.empty()) return std::numeric_limits<float>::infinity();
    float total = 0.0f;
    for (size_t index = 0; index < set.centers.size(); ++index) {
        total += patchSadCachedCurrentFractional(
            set.currentPatches[index], previous, set.centers[index].first,
            set.centers[index].second, dx, dy);
    }
    return total / static_cast<float>(set.centers.size());
}

struct GlobalTranslation {
    float dx = 0.0f;
    float dy = 0.0f;
    float meanError = 0.0f;
};

// Two-stage translation search around a starting candidate: an integer pass
// followed by a quarter-pixel pass. All evaluation uses the spread patch set
// so the result is a frame-level consensus rather than a single patch match.
GlobalTranslation refineGlobalTranslation(const LumaBuffer& current,
                                          const LumaBuffer& previous,
                                          uint32_t patchCount,
                                          float startDx, float startDy,
                                          int coarseRadius) {
    GlobalTranslation result;
    const AnchorPatchSet patches = buildAnchorPatches(current, patchCount);
    if (patches.centers.empty()) return result;
    float bestDx = std::round(startDx);
    float bestDy = std::round(startDy);
    float bestError = anchorSetSad(patches, previous, bestDx, bestDy);
    for (int oy = -coarseRadius; oy <= coarseRadius; ++oy) {
        for (int ox = -coarseRadius; ox <= coarseRadius; ++ox) {
            const float dx = std::round(startDx) + static_cast<float>(ox);
            const float dy = std::round(startDy) + static_cast<float>(oy);
            const float error = anchorSetSad(patches, previous, dx, dy);
            if (error < bestError) {
                bestError = error;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }
    result.dx = bestDx;
    result.dy = bestDy;
    result.meanError = bestError;
    // Quarter-pixel refinement around the integer winner. Analysis luma is
    // reduced-resolution, so this restores sub-source-pixel precision that
    // integer-only candidates quantize away.
    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            const float dx = bestDx + static_cast<float>(ox) * 0.25f;
            const float dy = bestDy + static_cast<float>(oy) * 0.25f;
            const float error = anchorSetSad(patches, previous, dx, dy);
            if (error < result.meanError) {
                result.meanError = error;
                result.dx = dx;
                result.dy = dy;
            }
        }
    }
    return result;
}

} // namespace

void MotionEstimator::beginFrame(bool sceneCut) {
    stats_ = {};
    stats_.sceneCut = sceneCut;
}

float MotionEstimator::aggregateConfidence(const std::vector<MvEntry>& mvs,
                                           int width, int height,
                                           float emptyConfidence,
                                           bool includeLocalConfidence) {
    // Aggregate only evidence that is valid in the source frame. The caller
    // uses this scalar to decide how much temporal history to trust, while the
    // uploader still preserves each block's local confidence in its validity
    // texture. Including both geometry and local confidence prevents sparse,
    // weakly matched fields from being promoted by coverage alone.
    const float empty = std::clamp(emptyConfidence, 0.0f, 1.0f);
    if (width <= 0 || height <= 0 || mvs.empty())
        return empty;

    const double frameArea = static_cast<double>(width) * height;
    double covered = 0.0;
    double weightedMagnitude = 0.0;
    double weightedMagnitudeSq = 0.0;
    double weightedConfidence = 0.0;
    for (const MvEntry& mv : mvs) {
        const int blockW = std::max(1, static_cast<int>(mv.w));
        const int blockH = std::max(1, static_cast<int>(mv.h));
        const int x0 = std::clamp(static_cast<int>(mv.dstX), 0, width);
        const int y0 = std::clamp(static_cast<int>(mv.dstY), 0, height);
        const int x1 = std::clamp(static_cast<int>(mv.dstX) + blockW, 0, width);
        const int y1 = std::clamp(static_cast<int>(mv.dstY) + blockH, 0, height);
        const double area = static_cast<double>(std::max(0, x1 - x0)) *
                            std::max(0, y1 - y0);
        if (area <= 0.0 || !std::isfinite(mv.mvX) ||
            !std::isfinite(mv.mvY) || !std::isfinite(mv.confidence))
            continue;
        const double magnitude =
            std::hypot(static_cast<double>(mv.mvX),
                       static_cast<double>(mv.mvY)) /
            std::hypot(static_cast<double>(width), static_cast<double>(height));
        covered += area;
        weightedMagnitude += area * magnitude;
        weightedMagnitudeSq += area * magnitude * magnitude;
        weightedConfidence += area * std::clamp(mv.confidence, 0.0f, 1.0f);
    }
    if (covered <= 0.0) return 0.25f;

    const double coverage = std::clamp(covered / frameArea, 0.0, 1.0);
    const double mean = weightedMagnitude / covered;
    const double variance = std::max(
        0.0, weightedMagnitudeSq / covered - mean * mean);
    const double consistency = 1.0 / (1.0 + 18.0 * std::sqrt(variance));
    const double displacementTrust = 1.0 / (1.0 + 2.0 * mean);
    const double localConfidence = includeLocalConfidence
        ? std::clamp(weightedConfidence / covered, 0.0, 1.0)
        : 1.0;
    return static_cast<float>(std::clamp(
        0.25 + 0.75 * coverage * consistency * displacementTrust *
            localConfidence,
        0.0, 1.0));
}

MotionEstimatorConfig MotionEstimator::configFromEnvironment() {
    MotionEstimatorConfig config;
    const char* mode = std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR");
    // The quality runner labels its causal arms with MOTION_ABLATION. Treat
    // refined as a real estimator request when the dedicated estimator
    // variable is absent, while preserving the dedicated variable's
    // precedence and leaving zero/block to their later payload ablations.
    if (!mode || !*mode)
        mode = std::getenv("TFORGE_FSR4_MOTION_ABLATION");
    if (mode && std::strcmp(mode, "codec") == 0)
        config.mode = MotionEstimatorMode::Codec;
    else if (mode && (std::strcmp(mode, "refined") == 0 ||
                      std::strcmp(mode, "codec_refined") == 0))
        config.mode = MotionEstimatorMode::CodecRefined;

    const int divisor = parseInt(std::getenv("TFORGE_FSR4_MOTION_REFINEMENT_SCALE"),
                                 4, 2, 8);
    config.refinementScale = divisor <= 2 ? 2u : divisor >= 8 ? 8u : 4u;
    config.searchRadius = parseInt(
        std::getenv("TFORGE_FSR4_MOTION_SEARCH_RADIUS"), 2, 0, 4);
    config.maxCorrectionPixels = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_MAX_CORRECTION"),
        static_cast<float>(config.refinementScale), 0.0f, 16.0f);
    config.minErrorImprovement = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_MIN_ERROR_IMPROVEMENT"), 0.0025f,
        0.0f, 1.0f);
    config.minErrorMargin = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_MIN_ERROR_MARGIN"), 0.001f, 0.0f,
        1.0f);
    config.maxRefinedSeeds = static_cast<uint32_t>(parseInt(
        std::getenv("TFORGE_FSR4_MOTION_MAX_REFINED_SEEDS"), 4096, 1,
        65536));
    config.confidenceErrorScale = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_CONFIDENCE_SCALE"), 0.04f, 0.001f, 1.0f);
    config.confidenceThreshold = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_CONFIDENCE_THRESHOLD"), 0.15f, 0.0f, 1.0f);
    config.sceneCutThreshold = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_SCENE_CUT_THRESHOLD"), 0.65f, 0.0f, 1.0f);
    const char *edgeAware = std::getenv("TFORGE_FSR4_MOTION_EDGE_AWARE");
    config.edgeAwareUpscale = edgeAware && std::strcmp(edgeAware, "0") != 0;
    config.allowFallbackAfterFiltering =
        std::getenv("TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING") != nullptr;
    config.denseGridFallback =
        std::getenv("TFORGE_FSR4_MOTION_DENSE_GRID") != nullptr;
    const char *globalAnchor = std::getenv("TFORGE_FSR4_MOTION_GLOBAL_ANCHOR");
    config.globalAnchor = globalAnchor && std::strcmp(globalAnchor, "0") != 0;
    config.globalAnchorSplitPixels = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_GLOBAL_ANCHOR_SPLIT"), 2.0f, 0.0f, 16.0f);
    config.globalAnchorMinAgreement = parseFloat(
        std::getenv("TFORGE_FSR4_MOTION_GLOBAL_ANCHOR_MIN_AGREEMENT"), 0.5f,
        0.0f, 1.0f);
    config.globalAnchorPatches = static_cast<uint32_t>(parseInt(
        std::getenv("TFORGE_FSR4_MOTION_GLOBAL_ANCHOR_PATCHES"), 64, 1, 256));
    return config;
}

std::vector<MvEntry> MotionEstimator::estimate(
    const MotionEstimatorConfig& config, const LumaBuffer& current,
    const LumaBuffer& previous, const std::vector<MvEntry>& codecSeeds,
    uint32_t sourceWidth, uint32_t sourceHeight) {
    const auto started = std::chrono::steady_clock::now();
    stats_.inputSeeds = static_cast<uint32_t>(codecSeeds.size());
    if (config.mode == MotionEstimatorMode::Off || stats_.sceneCut ||
        current.width == 0 || current.height == 0 || previous.width != current.width ||
        previous.height != current.height || current.data.empty() ||
        previous.data.empty() || sourceWidth == 0 || sourceHeight == 0) {
        stats_.cpuMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return {};
    }

    std::vector<MvEntry> output;
    output.reserve(codecSeeds.size());
    std::vector<float> xs;
    std::vector<float> ys;
    xs.reserve(codecSeeds.size());
    ys.reserve(codecSeeds.size());
    for (const MvEntry& seed : codecSeeds) {
        // The causal player admits only the explicit immediately-previous
        // marker. FFmpeg's source field does not prove arbitrary older
        // references are usable for this frame's history reprojection, so
        // keep the estimator contract aligned with PlaybackEngine.
        if (seed.source != -1 || !std::isfinite(seed.mvX) ||
            !std::isfinite(seed.mvY) || !std::isfinite(seed.confidence))
            continue;
        output.push_back(seed);
        xs.push_back(seed.mvX);
        ys.push_back(seed.mvY);
    }
    stats_.acceptedSeeds = static_cast<uint32_t>(output.size());
    const size_t nonZeroSeeds = std::count_if(
        output.begin(), output.end(), [](const MvEntry& entry) {
            return std::abs(entry.mvX) > 0.001f || std::abs(entry.mvY) > 0.001f;
        });
    const bool sparseCodecCoverage =
        output.size() > 0 && nonZeroSeeds * 32u < output.size();
    if (config.denseGridFallback &&
        config.mode == MotionEstimatorMode::CodecRefined &&
        previous.width == current.width && previous.height == current.height &&
        (output.empty() || sparseCodecCoverage)) {
            // Keep trusted codec blocks after the inferred fill field. The GPU
            // expansion uses deterministic last-writer ownership, so this
            // ordering makes dense discovery additive instead of allowing a
            // noisier luma estimate to replace a valid codec correspondence.
            std::vector<MvEntry> trustedCodecSeeds = std::move(output);
            output.clear();
            output.reserve(trustedCodecSeeds.size() +
                           static_cast<size_t>(sourceWidth / 2u + 1u) *
                               static_cast<size_t>(sourceHeight / 2u + 1u));
            // Codec metadata can omit independently moving objects entirely.
            // Search one source-space pixel around each reduced-resolution
            // grid sample so the fallback can discover object motion without
            // pretending a frame-wide translation explains the whole image.
            const uint32_t step = std::clamp(config.refinementScale, 1u, 8u);
            const int radius = std::clamp(config.searchRadius, 0, 4);
            const float scaleX = static_cast<float>(current.width) /
                                 static_cast<float>(sourceWidth);
            const float scaleY = static_cast<float>(current.height) /
                                 static_cast<float>(sourceHeight);
            for (uint32_t sy = 0; sy < sourceHeight; sy += step) {
                for (uint32_t sx = 0; sx < sourceWidth; sx += step) {
                    const int x = std::clamp(
                        static_cast<int>((sx + step / 2u) * scaleX), 0,
                        static_cast<int>(current.width) - 1);
                    const int y = std::clamp(
                        static_cast<int>((sy + step / 2u) * scaleY), 0,
                        static_cast<int>(current.height) - 1);
                    std::array<float, 9> currentPatch{};
                    size_t patchIndex = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox, ++patchIndex)
                            currentPatch[patchIndex] =
                                sample(current, x + ox, y + oy);
                    float bestError = std::numeric_limits<float>::infinity();
                    float secondBestError = std::numeric_limits<float>::infinity();
                    float bestDx = 0.0f;
                    float bestDy = 0.0f;
                    for (int dyIndex = -radius * 2; dyIndex <= radius * 2;
                         ++dyIndex) {
                        for (int dxIndex = -radius * 2; dxIndex <= radius * 2;
                             ++dxIndex) {
                            const float dx = static_cast<float>(dxIndex) * 0.5f;
                            const float dy = static_cast<float>(dyIndex) * 0.5f;
                            const float error = patchSadCachedCurrentFractional(
                                currentPatch, previous, x, y, dx, dy);
                            if (error < bestError) {
                                secondBestError = bestError;
                                bestError = error;
                                bestDx = dx;
                                bestDy = dy;
                            } else if (error < secondBestError) {
                                secondBestError = error;
                            }
                        }
                    }
                    const float zeroError = patchSadCachedCurrentFractional(
                        currentPatch, previous, x, y, 0, 0);
                    float reverseBestError =
                        std::numeric_limits<float>::infinity();
                    float reverseDx = 0.0f;
                    float reverseDy = 0.0f;
                    for (int dyIndex = -radius * 2; dyIndex <= radius * 2;
                         ++dyIndex) {
                        for (int dxIndex = -radius * 2; dxIndex <= radius * 2;
                             ++dxIndex) {
                            const float candidateDx =
                                -bestDx + static_cast<float>(dxIndex) * 0.5f;
                            const float candidateDy =
                                -bestDy + static_cast<float>(dyIndex) * 0.5f;
                            const float error = patchSadBilinear(
                                previous, current,
                                static_cast<float>(x) + bestDx,
                                static_cast<float>(y) + bestDy,
                                candidateDx, candidateDy);
                            if (error < reverseBestError) {
                                reverseBestError = error;
                                reverseDx = candidateDx;
                                reverseDy = candidateDy;
                            }
                        }
                    }
                    const float forwardBackwardError = std::hypot(
                        bestDx + reverseDx, bestDy + reverseDy);
                    // A flat patch produces equal SAD for every displacement.
                    // Treat that tie as no evidence instead of selecting the
                    // first loop candidate (which would create a systematic
                    // diagonal vector across the entire flat image).
                    const float minimumImprovement = std::max(
                        0.005f, config.minErrorImprovement);
                    const float minimumMargin = std::max(
                        0.01f, config.minErrorMargin);
                    const bool hasMotionEvidence =
                        zeroError - bestError >= minimumImprovement &&
                        secondBestError - bestError >= minimumMargin &&
                        forwardBackwardError <= 0.75f;
                    if (!hasMotionEvidence) {
                        bestDx = 0;
                        bestDy = 0;
                        bestError = zeroError;
                    }
                    MvEntry grid;
                    grid.dstX = static_cast<int16_t>(sx);
                    grid.dstY = static_cast<int16_t>(sy);
                    grid.w = static_cast<uint8_t>(std::min(
                        step, sourceWidth - sx));
                    grid.h = static_cast<uint8_t>(std::min(
                        step, sourceHeight - sy));
                    grid.mvX = bestDx /
                               std::max(scaleX, 1.0e-6f);
                    grid.mvY = bestDy /
                               std::max(scaleY, 1.0e-6f);
                    grid.source = -1;
                    grid.confidence = hasMotionEvidence
                        ? std::clamp(
                              std::exp(-bestError /
                                       std::max(0.001f,
                                                config.confidenceErrorScale)) *
                                  0.5f,
                              0.0f, 0.5f)
                        : 0.0f;
                    output.push_back(grid);
                    stats_.meanResidual += bestError;
                }
            }
            stats_.acceptedSeeds = static_cast<uint32_t>(output.size());
            stats_.meanResidual /=
                static_cast<float>(std::max<size_t>(1, output.size()));
            stats_.meanConfidence = aggregateConfidence(
                output, static_cast<int>(sourceWidth),
                static_cast<int>(sourceHeight), 0.0f, true);
            // Place validated codec entries last, preserving their original
            // coverage and motion values over approximate dense candidates.
            output.insert(output.end(), trustedCodecSeeds.begin(),
                          trustedCodecSeeds.end());
            stats_.cpuMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return output;
    }
    if (output.empty()) {
        // A single robust global translation is preferable to an invented
        // block field when the codec exports no usable causal vectors. It is
        // deliberately low-confidence and only covers the refined mode;
        // the caller's existing history gate can reject it conservatively.
        if (config.mode != MotionEstimatorMode::Off &&
            (config.mode == MotionEstimatorMode::CodecRefined ||
             config.allowFallbackAfterFiltering) &&
            (codecSeeds.empty() || config.allowFallbackAfterFiltering) &&
            previous.width == current.width &&
            previous.height == current.height) {
            // Judge the fallback against the same spread patch set as the
            // camera anchor instead of one center patch: a single 3x3 patch
            // cannot distinguish translation from unrelated texture, and a
            // flat center would tie every candidate.
            const GlobalTranslation fallback = refineGlobalTranslation(
                current, previous,
                std::max<uint32_t>(config.globalAnchorPatches, 64u), 0.0f,
                0.0f,
                std::min(16, static_cast<int>(std::min(current.width,
                                                       current.height) /
                                              4u)));
            const float bestError = fallback.meanError;
            const float scaleX = static_cast<float>(current.width) /
                                 static_cast<float>(sourceWidth);
            const float scaleY = static_cast<float>(current.height) /
                                 static_cast<float>(sourceHeight);
            const float globalMvX = fallback.dx /
                                    std::max(scaleX, 1.0e-6f);
            const float globalMvY = fallback.dy /
                                    std::max(scaleY, 1.0e-6f);
            // This vector is estimated from the immediately previous decoded
            // luma frame, not copied from a codec reference-picture entry.
            // Keep the causal fallback marker explicit so downstream motion
            // adapters cannot mistake it for an unverified codec reference.
            // Its deliberately capped confidence remains the indication that
            // this global estimate is weaker than a validated codec seed.
            const float globalConfidence = std::clamp(
                std::exp(-bestError /
                         std::max(0.001f, config.confidenceErrorScale)) * 0.5f,
                0.0f, 0.5f);
            // MvEntry preserves FFmpeg's uint8 block dimensions. Tile the
            // full source frame instead of clamping one synthetic block to
            // 255x255; otherwise the fallback only covers the upper-left
            // corner and the remaining pixels silently become zero motion.
            constexpr uint32_t kMaxFallbackTile = 255u;
            for (uint32_t tileY = 0; tileY < sourceHeight;
                 tileY += kMaxFallbackTile) {
                for (uint32_t tileX = 0; tileX < sourceWidth;
                     tileX += kMaxFallbackTile) {
                    MvEntry global;
                    global.dstX = static_cast<int16_t>(tileX);
                    global.dstY = static_cast<int16_t>(tileY);
                    global.w = static_cast<uint8_t>(std::min(
                        kMaxFallbackTile, sourceWidth - tileX));
                    global.h = static_cast<uint8_t>(std::min(
                        kMaxFallbackTile, sourceHeight - tileY));
                    global.mvX = globalMvX;
                    global.mvY = globalMvY;
                    global.source = -1;
                    global.confidence = globalConfidence;
                    output.push_back(global);
                }
            }
            stats_.acceptedSeeds = static_cast<uint32_t>(output.size());
            stats_.dominantMotionX = globalMvX;
            stats_.dominantMotionY = globalMvY;
            stats_.meanResidual = bestError;
            stats_.meanConfidence = globalConfidence;
            if (globalConfidence < config.confidenceThreshold)
                stats_.lowConfidenceSeeds = stats_.acceptedSeeds;
        }
        stats_.cpuMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return output;
    }

    auto median = [](std::vector<float>& values) {
        const auto mid = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), mid, values.end());
        return *mid;
    };
    stats_.dominantMotionX = median(xs);
    stats_.dominantMotionY = median(ys);

    if (config.mode == MotionEstimatorMode::Codec) {
        float confidenceSum = 0.0f;
        for (const MvEntry& entry : output) confidenceSum += entry.confidence;
        stats_.meanConfidence = confidenceSum / static_cast<float>(output.size());
        stats_.cpuMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return output;
    }

    const float scaleX = static_cast<float>(current.width) /
                         static_cast<float>(sourceWidth);
    const float scaleY = static_cast<float>(current.height) /
                         static_cast<float>(sourceHeight);
    const int radius = std::clamp(config.searchRadius, 0, 4);
    // Camera-translation consensus: refine the median seed into a sub-pixel
    // frame-level translation and replace agreeing seeds with it. Block-level
    // vectors wobble frame-to-frame even during a steady camera move, and
    // that wobble transfers directly into reprojection flicker. Seeds that
    // disagree with the anchor beyond the split threshold keep their own
    // vector so independently moving objects are not flattened into the
    // camera model.
    std::vector<bool> anchored(output.size(), false);
    if (config.globalAnchor) {
        const GlobalTranslation refinedAnchor = refineGlobalTranslation(
            current, previous, config.globalAnchorPatches,
            stats_.dominantMotionX * scaleX, stats_.dominantMotionY * scaleY,
            1);
        const float anchorMvX =
            refinedAnchor.dx / std::max(scaleX, 1.0e-6f);
        const float anchorMvY =
            refinedAnchor.dy / std::max(scaleY, 1.0e-6f);
        size_t agreeingSeeds = 0;
        for (const MvEntry& entry : output) {
            if (std::hypot(entry.mvX - anchorMvX, entry.mvY - anchorMvY) <=
                config.globalAnchorSplitPixels)
                ++agreeingSeeds;
        }
        const float agreementFraction =
            static_cast<float>(agreeingSeeds) /
            static_cast<float>(output.size());
        if (agreementFraction >= config.globalAnchorMinAgreement) {
            for (size_t entryIndex = 0; entryIndex < output.size();
                 ++entryIndex) {
                MvEntry& entry = output[entryIndex];
                if (std::hypot(entry.mvX - anchorMvX,
                               entry.mvY - anchorMvY) <=
                    config.globalAnchorSplitPixels) {
                    entry.mvX = anchorMvX;
                    entry.mvY = anchorMvY;
                    anchored[entryIndex] = true;
                    ++stats_.anchoredSeeds;
                }
            }
            stats_.anchorApplied = true;
            stats_.anchorX = anchorMvX;
            stats_.anchorY = anchorMvY;
            stats_.anchorMeanResidual = refinedAnchor.meanError;
        }
    }
    const uint32_t maxRefinedSeeds = std::max(1u, config.maxRefinedSeeds);
    // Codec exports can contain tens of thousands of small blocks at 1080p.
    // Spread a bounded number of searches through that list rather than
    // refining only its first region. Unsampled blocks retain their validated
    // codec vector and remain in the coverage field.
    const size_t refinementStride =
        output.size() > maxRefinedSeeds
            ? (output.size() + maxRefinedSeeds - 1u) / maxRefinedSeeds
            : 1u;
    float residualSum = 0.0f;
    float confidenceSum = 0.0f;
    uint32_t refinedSampleCount = 0;
    for (size_t entryIndex = 0; entryIndex < output.size(); ++entryIndex) {
        MvEntry& entry = output[entryIndex];
        if ((entryIndex % refinementStride) != 0)
            continue;
        ++refinedSampleCount;
        const int x = std::clamp(static_cast<int>(std::lround(
            (static_cast<float>(entry.dstX) + static_cast<float>(entry.w) * 0.5f) * scaleX)),
            0, static_cast<int>(current.width) - 1);
        const int y = std::clamp(static_cast<int>(std::lround(
            (static_cast<float>(entry.dstY) + static_cast<float>(entry.h) * 0.5f) * scaleY)),
            0, static_cast<int>(current.height) - 1);
        const int seedDx = static_cast<int>(std::lround(entry.mvX * scaleX));
        const int seedDy = static_cast<int>(std::lround(entry.mvY * scaleY));
        std::array<float, 9> currentPatch{};
        size_t patchIndex = 0;
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox, ++patchIndex)
                currentPatch[patchIndex] = sample(current, x + ox, y + oy);
        if (anchored[entryIndex]) {
            // Anchored seeds share the frame-level consensus translation, so
            // the per-seed local search is skipped: reintroducing independent
            // integer corrections here would rebuild the very field wobble
            // the anchor removed. Confidence still comes from this block's
            // own photometric evidence at the anchored position, evaluated
            // with fractional sampling because the anchor is sub-pixel.
            const float anchoredError = patchSadCachedCurrentFractional(
                currentPatch, previous, x, y,
                entry.mvX * scaleX, entry.mvY * scaleY);
            const float anchoredConfidence = std::clamp(
                std::exp(-anchoredError /
                         std::max(0.001f, config.confidenceErrorScale)),
                0.0f, 1.0f);
            entry.confidence = std::min(entry.confidence, anchoredConfidence);
            residualSum += anchoredError;
            confidenceSum += entry.confidence;
            if (entry.confidence < config.confidenceThreshold)
                ++stats_.lowConfidenceSeeds;
            continue;
        }
        const float seedError = patchSadCachedCurrent(
            currentPatch, previous, x, y, seedDx, seedDy);
        float bestError = seedError;
        int bestDx = seedDx;
        int bestDy = seedDy;
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                const int candidateDx = seedDx + ox;
                const int candidateDy = seedDy + oy;
                const float error = patchSadCachedCurrent(
                    currentPatch, previous, x, y, candidateDx, candidateDy);
                const int oldDistance = std::abs(bestDx - seedDx) +
                                         std::abs(bestDy - seedDy);
                const int newDistance = std::abs(ox) + std::abs(oy);
                if (error < bestError - 1e-6f ||
                    (std::abs(error - bestError) <= 1e-6f &&
                     newDistance < oldDistance)) {
                    bestError = error;
                    bestDx = candidateDx;
                    bestDy = candidateDy;
                }
            }
        }
        float secondBestError = std::numeric_limits<float>::infinity();
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                const int candidateDx = seedDx + ox;
                const int candidateDy = seedDy + oy;
                if (candidateDx == bestDx && candidateDy == bestDy)
                    continue;
                secondBestError = std::min(
                    secondBestError,
                    patchSadCachedCurrent(currentPatch, previous, x, y,
                                          candidateDx, candidateDy));
            }
        }
        const float correctionX =
            static_cast<float>(bestDx - seedDx) / scaleX;
        const float correctionY =
            static_cast<float>(bestDy - seedDy) / scaleY;
        const bool correctionAccepted =
            (bestDx == seedDx && bestDy == seedDy) ||
            (bestError <= seedError - std::max(0.0f,
                                               config.minErrorImprovement) &&
             secondBestError - bestError >=
                 std::max(0.0f, config.minErrorMargin) &&
             std::hypot(correctionX, correctionY) <=
                std::max(0.0f, config.maxCorrectionPixels));
        if (correctionAccepted) {
            if (bestDx != seedDx || bestDy != seedDy)
                ++stats_.refinedSeeds;
            entry.mvX += correctionX;
            entry.mvY += correctionY;
        } else {
            // A low-resolution search can produce a numerically better but
            // spatially implausible jump. Keep the validated codec seed and
            // lower trust; the caller's existing history gate can reject it.
            entry.confidence *= 0.5f;
        }
        const float confidence = std::clamp(
            std::exp(-bestError / std::max(0.001f, config.confidenceErrorScale)),
            0.0f, 1.0f);
        entry.confidence = std::min(entry.confidence, confidence);
        residualSum += bestError;
        confidenceSum += entry.confidence;
        if (entry.confidence < config.confidenceThreshold)
            ++stats_.lowConfidenceSeeds;
    }
    stats_.meanResidual = refinedSampleCount > 0
                              ? residualSum / static_cast<float>(refinedSampleCount)
                              : 0.0f;
    stats_.meanConfidence = refinedSampleCount > 0
                                ? confidenceSum / static_cast<float>(refinedSampleCount)
                                : 0.0f;
    stats_.cpuMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return output;
}

} // namespace temporal_forge
