// motion_estimator_tests.cpp — deterministic tests for the cheap causal
// estimator. These tests exercise the estimator before FSR so a temporal
// failure cannot be hidden by a plausible final image.
#include "motion/MotionEstimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace temporal_forge;

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (false)

static LumaBuffer gradient(uint32_t width, uint32_t height, int shift) {
    LumaBuffer out{width, height, std::vector<float>(width * height)};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const int sourceX = std::clamp(static_cast<int>(x) - shift, 0,
                                           static_cast<int>(width) - 1);
            out.data[static_cast<size_t>(y) * width + x] =
                static_cast<float>((sourceX * 7 + static_cast<int>(y) * 13) % 97) / 96.0f;
        }
    }
    return out;
}

int main() {
    // The campaign runner uses MOTION_ABLATION labels. "refined" must map
    // to the standalone refined estimator instead of silently falling back
    // to the legacy codec path, or the capture label lies about the work run.
    unsetenv("TFORGE_FSR4_MOTION_ESTIMATOR");
    setenv("TFORGE_FSR4_MOTION_ABLATION", "refined", 1);
    const auto refinedEnvironmentConfig =
        MotionEstimator::configFromEnvironment();
    CHECK(refinedEnvironmentConfig.mode == MotionEstimatorMode::CodecRefined);
    unsetenv("TFORGE_FSR4_MOTION_ABLATION");

    // Frame-level history trust must include the per-block confidence that
    // validation/refinement computed. Otherwise a field with full geometric
    // coverage but mostly rejected blocks still looks as trustworthy as a
    // clean field and the downstream temporal history gate cannot help.
    MvEntry trustedBlock;
    trustedBlock.dstX = 0;
    trustedBlock.dstY = 0;
    trustedBlock.w = 32;
    trustedBlock.h = 16;
    trustedBlock.source = -1;
    trustedBlock.confidence = 1.0f;
    MvEntry uncertainBlock = trustedBlock;
    uncertainBlock.confidence = 0.1f;
    const float trustedFrameConfidence = MotionEstimator::aggregateConfidence(
        {trustedBlock}, 32, 16);
    const float uncertainFrameConfidence = MotionEstimator::aggregateConfidence(
        {uncertainBlock}, 32, 16);
    CHECK(uncertainFrameConfidence < trustedFrameConfidence - 0.1f);
    const float legacyTrustedConfidence = MotionEstimator::aggregateConfidence(
        {trustedBlock}, 32, 16, 0.5f, false);
    const float legacyUncertainConfidence = MotionEstimator::aggregateConfidence(
        {uncertainBlock}, 32, 16, 0.5f, false);
    CHECK(std::fabs(legacyTrustedConfidence - legacyUncertainConfidence) <
          1e-6f);

    const LumaBuffer previous = gradient(32, 16, 0);
    const LumaBuffer current = gradient(32, 16, 1);
    MvEntry seed;
    seed.dstX = 4;
    seed.dstY = 4;
    seed.w = 16;
    seed.h = 8;
    seed.source = -1;
    seed.confidence = 1.0f;

    MotionEstimatorConfig config;
    config.mode = MotionEstimatorMode::CodecRefined;
    config.searchRadius = 2;
    config.confidenceErrorScale = 0.2f;
    // The default quarter-resolution grid moves in four-source-pixel steps;
    // a one-pixel default bound would make every nonzero correction
    // impossible and turn the refined estimator into codec-only motion.
    CHECK(config.maxCorrectionPixels >= 4.0f);
    MotionEstimator estimator;
    estimator.beginFrame(false);
    const auto refined = estimator.estimate(config, current, previous, {seed},
                                            32, 16);
    CHECK(refined.size() == 1);
    CHECK(refined[0].mvX < -0.5f);
    CHECK(std::isfinite(refined[0].confidence));
    CHECK(estimator.stats().refinedSeeds > 0);

    // A reduced analysis grid must not turn an ambiguous local-SAD winner
    // into a multi-pixel source-space jump. Keep the codec seed when the
    // candidate correction exceeds the conservative source-pixel bound.
    MotionEstimatorConfig boundedConfig = config;
    boundedConfig.maxCorrectionPixels = 1.0f;
    const LumaBuffer shiftedTwo = gradient(32, 16, 2);
    MvEntry zeroSeed = seed;
    zeroSeed.mvX = 0.0f;
    zeroSeed.mvY = 0.0f;
    estimator.beginFrame(false);
    const auto bounded = estimator.estimate(
        boundedConfig, shiftedTwo, previous, {zeroSeed}, 32, 16);
    CHECK(bounded.size() == 1);
    CHECK(std::hypot(bounded[0].mvX, bounded[0].mvY) <= 1.0f + 1e-5f);

    // A numerically lower candidate is not enough on its own. A deliberately
    // larger improvement threshold must preserve the codec seed, proving the
    // acceptance policy can reject weak local-SAD wins independently of the
    // source-space correction bound.
    MotionEstimatorConfig strictConfig = config;
    strictConfig.minErrorImprovement = 1.0f;
    MvEntry onePixelSeed = seed;
    onePixelSeed.mvX = 0.0f;
    estimator.beginFrame(false);
    const auto strict = estimator.estimate(
        strictConfig, current, previous, {onePixelSeed}, 32, 16);
    CHECK(strict.size() == 1);
    CHECK(std::fabs(strict[0].mvX) < 1e-5f);

    // A global estimate is still computed from the immediately previous
    // decoded luma frame. It must carry the same causal marker as codec seeds;
    // source=0 is ambiguous and lets downstream adapters mistake the estimate
    // for an unverified reference.
    estimator.beginFrame(false);
    const auto globalFallback = estimator.estimate(
        config, current, previous, {}, 32, 16);
    CHECK(!globalFallback.empty());
    CHECK(globalFallback.front().source == -1);
    CHECK(globalFallback.front().confidence <= 0.5f + 1e-6f);

    // A full-frame fallback must be represented by tiles because MvEntry's
    // codec-compatible block dimensions are uint8_t. One clamped 255x255
    // entry only covers the upper-left corner of normal video and leaves the
    // rest of the temporal field with no usable fallback motion.
    const LumaBuffer largePrevious = gradient(640, 360, 0);
    const LumaBuffer largeCurrent = gradient(640, 360, 1);
    estimator.beginFrame(false);
    const auto tiledFallback = estimator.estimate(
        config, largeCurrent, largePrevious, {}, 640, 360);
    CHECK(tiledFallback.size() == 6);
    CHECK(tiledFallback.back().dstX == 510);
    CHECK(tiledFallback.back().dstY == 255);
    CHECK(tiledFallback.back().w == 130);
    CHECK(tiledFallback.back().h == 105);

    // The estimator must keep a bounded refinement budget at high source
    // resolutions. The codec field remains complete; only the expensive
    // local-search correction is sampled across the seed list.
    std::vector<MvEntry> manySeeds(8, seed);
    config.maxRefinedSeeds = 2;
    estimator.beginFrame(false);
    const auto budgeted = estimator.estimate(config, current, previous,
                                             manySeeds, 32, 16);
    CHECK(budgeted.size() == manySeeds.size());
    CHECK(estimator.stats().refinedSeeds <= config.maxRefinedSeeds);

    estimator.beginFrame(true);
    CHECK(estimator.estimate(config, current, previous, {seed}, 32, 16).empty());

    seed.source = 1;
    config.allowFallbackAfterFiltering = false;
    estimator.beginFrame(false);
    CHECK(estimator.estimate(config, current, previous, {seed}, 32, 16).empty());

    // The causal adapter has one explicit supported reference marker. An
    // ambiguous zero or older negative reference must not enter refinement as
    // though it described the immediately previous displayed frame.
    for (const int8_t source : {int8_t{0}, int8_t{-2}}) {
        seed.source = source;
        estimator.beginFrame(false);
        CHECK(estimator.estimate(config, current, previous, {seed}, 32, 16).empty());
    }

    // Metadata can exist without containing a usable immediately-previous
    // reference: for example, a B-frame may expose only a future or older
    // reference. The estimator must combine causal filtering with its cheap
    // global fallback instead of returning an empty field merely because the
    // original metadata vector was non-empty. Downstream: the GPU expander
    // receives conservative full-frame correspondence rather than silently
    // losing temporal data for that frame.
    seed.source = 1;
    config.allowFallbackAfterFiltering = true;
    estimator.beginFrame(false);
    const auto filteredFallback = estimator.estimate(
        config, current, previous, {seed}, 32, 16);
    CHECK(!filteredFallback.empty());

    // The combined production profile explicitly selects codec-only motion.
    // Its conservative fallback must therefore work in Codec mode too; a
    // mode-specific guard would make the advertised fallback silently inert.
    MotionEstimatorConfig codecFallbackConfig = config;
    codecFallbackConfig.mode = MotionEstimatorMode::Codec;
    codecFallbackConfig.allowFallbackAfterFiltering = true;
    estimator.beginFrame(false);
    const auto codecFilteredFallback = estimator.estimate(
        codecFallbackConfig, current, previous, {seed}, 32, 16);
    CHECK(!codecFilteredFallback.empty());
    CHECK(codecFilteredFallback.front().source == -1);
    CHECK(filteredFallback.front().source == -1);
    CHECK(filteredFallback.front().confidence <= 0.5f + 1e-6f);

    // The seed-only estimator cannot discover motion in an uncovered region.
    // The opt-in dense grid must search that region independently and recover
    // the known one-pixel translation in this deterministic fixture.
    MotionEstimatorConfig denseConfig = config;
    denseConfig.denseGridFallback = true;
    denseConfig.refinementScale = 4;
    denseConfig.searchRadius = 2;
    denseConfig.maxCorrectionPixels = 8.0f;
    estimator.beginFrame(false);
    LumaBuffer blockPrevious{32, 16, std::vector<float>(32 * 16, 0.0f)};
    LumaBuffer blockCurrent{32, 16, std::vector<float>(32 * 16, 0.0f)};
    for (int y = 4; y < 10; ++y) {
        for (int x = 4; x < 10; ++x) {
            blockPrevious.data[static_cast<size_t>(y) * 32 + x] = 1.0f;
        }
        for (int x = 6; x < 12; ++x) {
            blockCurrent.data[static_cast<size_t>(y) * 32 + x] = 1.0f;
        }
    }
    // Exercise the real failure mode too: a sparse codec list can contain a
    // few accepted blocks while still leaving the moving region uncovered.
    MvEntry sparseSeed{};
    sparseSeed.w = 4;
    sparseSeed.h = 4;
    sparseSeed.source = -1;
    sparseSeed.confidence = 1.0f;
    MvEntry trustedSeed = sparseSeed;
    trustedSeed.dstX = 8;
    trustedSeed.dstY = 4;
    trustedSeed.mvX = -2.0f;
    trustedSeed.mvY = 0.0f;
    std::vector<MvEntry> sparseCodecSeeds(40, sparseSeed);
    sparseCodecSeeds.push_back(trustedSeed);
    const auto dense = estimator.estimate(
        denseConfig, blockCurrent, blockPrevious, sparseCodecSeeds, 32, 16);
    CHECK(!dense.empty());
    bool foundTranslatedCell = false;
    bool foundUntexturedCell = false;
    for (const MvEntry &entry : dense) {
        if (entry.dstX <= 8 && entry.dstX + entry.w > 8 &&
            entry.dstY <= 6 && entry.dstY + entry.h > 6 &&
            entry.mvX < -0.5f) {
            foundTranslatedCell = true;
        }
        if (entry.dstX >= 20 && entry.dstY >= 4 &&
            std::abs(entry.mvX) < 1e-6f && std::abs(entry.mvY) < 1e-6f &&
            entry.confidence < 1e-6f)
            foundUntexturedCell = true;
    }
    CHECK(foundTranslatedCell);
    // Uniform regions have no correspondence evidence. They must not select
    // the first tied search candidate and emit a false diagonal motion.
    CHECK(foundUntexturedCell);
    // Dense fill must not overwrite an already trusted codec block. The
    // codec seed is appended after fill entries so the GPU's deterministic
    // last-writer rule preserves it.
    CHECK(dense.back().dstX == trustedSeed.dstX &&
          dense.back().dstY == trustedSeed.dstY &&
          std::abs(dense.back().mvX - trustedSeed.mvX) < 1e-6f);
    return failures == 0 ? 0 : 1;
}
