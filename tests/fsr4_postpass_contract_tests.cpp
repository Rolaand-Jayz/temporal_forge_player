// fsr4_postpass_contract_tests.cpp — M1 tests written before implementation.
//
// These tests define the recovered v4.1 postpass parameter contract without
// judging image quality. They protect the byte range, endian/finite decode,
// and shader-consumption evidence needed before a visual experiment is valid.
#include "backend/Fsr4PostpassParams.hpp"
#include "backend/WeightBlob.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static void writeFloat(std::array<uint8_t, kFsr4BlobSize>& blob,
                       size_t index, float value) {
    std::memcpy(blob.data() + kFsr4PostpassParamOffset + index * sizeof(float),
                &value, sizeof(value));
}

static std::string readPostpassShader() {
#ifdef TFORGE_SOURCE_ROOT
    const std::filesystem::path path =
        std::filesystem::path(TFORGE_SOURCE_ROOT) /
        "shaders/fsr4/postpass_composite.comp";
#else
    const std::filesystem::path path = "shaders/fsr4/postpass_composite.comp";
#endif
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static std::string readSourceFile(const char *relativePath) {
#ifdef TFORGE_SOURCE_ROOT
    const std::filesystem::path path =
        std::filesystem::path(TFORGE_SOURCE_ROOT) / relativePath;
#else
    const std::filesystem::path path = relativePath;
#endif
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

static std::string extractFunctionBody(const std::string &shader,
                                       const std::string &signature) {
    const size_t start = shader.find(signature);
    if (start == std::string::npos)
        return {};
    const size_t open = shader.find('{', start);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t index = open; index < shader.size(); ++index) {
        if (shader[index] == '{')
            ++depth;
        else if (shader[index] == '}' && --depth == 0)
            return shader.substr(open, index - open + 1);
    }
    return {};
}

int main() {
    CHECK(kFsr4PostpassParamOffset == 130088);
    CHECK(kFsr4PostpassParamCount == 222);
    CHECK(kFsr4PostpassParamBytes == 222 * sizeof(float));
    CHECK(kFsr4PostpassParamOffset + kFsr4PostpassParamBytes == kFsr4PadZoneOffset);

    std::array<uint8_t, kFsr4BlobSize> blob{};
    writeFloat(blob, 0, 1.25f);
    writeFloat(blob, 1, -0.5f);
    writeFloat(blob, kFsr4PostpassParamCount - 1, 42.0f);
    const auto decoded = decodeFsr4PostpassParams(blob.data(), blob.size());
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK((*decoded)[0] == 1.25f);
        CHECK((*decoded)[1] == -0.5f);
        CHECK((*decoded)[kFsr4PostpassParamCount - 1] == 42.0f);
    }

    CHECK(!decodeFsr4PostpassParams(blob.data(), kFsr4PostpassParamOffset +
                                    kFsr4PostpassParamBytes - 1).has_value());
    writeFloat(blob, 4, NAN);
    CHECK(!decodeFsr4PostpassParams(blob.data(), blob.size()).has_value());

    const std::string shader = readPostpassShader();
    const std::string harnessSource =
        readSourceFile("src/render/Fsr4DispatchHarness.cpp");
    const std::string playbackQualitySource =
        readSourceFile("src/core/PlaybackEngine.cpp");
    CHECK(!shader.empty());
    // The measured base-only candidate is scale-aware: it is the normal
    // policy only for severe upscales, while explicit lab JSON remains able
    // to request any composition for a controlled experiment.
    CHECK(playbackQualitySource.find("defaultScaleAwareQualityLab") !=
          std::string::npos);
    CHECK(playbackQualitySource.find("TFORGE_QUALITY_LAB_CONFIG") !=
          std::string::npos);
    CHECK(playbackQualitySource.find("passScale < 3.0f") != std::string::npos);
    CHECK(playbackQualitySource.find("passQualityLabConfig.enabled = false") !=
          std::string::npos);
    // A true 1:1 selection must preserve the uploaded native image instead of
    // sending it through neural reconstruction. The runtime contract also
    // requires the native path to publish the raw image and finish uploads
    // synchronously before the render thread can consume it.
    CHECK(playbackQualitySource.find("nativePassthrough") !=
          std::string::npos);
    CHECK(playbackQualitySource.find("fsr4NativePassthrough_") !=
          std::string::npos);
    CHECK(playbackQualitySource.find("rawPresentationImage()") !=
          std::string::npos);
    CHECK(playbackQualitySource.find(
              "endFrameUploads(nativePassthrough ? nullptr") !=
          std::string::npos);
    CHECK(playbackQualitySource.find(
              "std::vector<MvEntry> temporalMotion") !=
          std::string::npos);
    const size_t nativeMotionGuard =
        playbackQualitySource.find("if (!nativePassthrough)");
    CHECK(nativeMotionGuard != std::string::npos);
    if (nativeMotionGuard != std::string::npos) {
        const size_t nativeMotionUpload =
            playbackQualitySource.find("uploadMotion", nativeMotionGuard);
        CHECK(nativeMotionUpload != std::string::npos);
    }
    const size_t declaration = shader.find("float weightParams[]");
    CHECK(declaration != std::string::npos);
    CHECK(shader.find("weightParams[", declaration + 1) != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_PARAM_OFFSET") != std::string::npos);
    CHECK(shader.find("postpassParameterTrace") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_OUTPUT_BIAS0") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_OUTPUT_BIAS1") == std::string::npos);
    CHECK(shader.find("recurrentLogits + recurrentBias") != std::string::npos);
    CHECK(shader.find("slot0.z & 4096u") != std::string::npos);
    CHECK(shader.find("slot0.z & 64u") != std::string::npos);

    // The postpass-tail experiment is host-only plumbing.  By default the
    // existing bit-64 enable must remain active; the opt-in environment
    // variable may suppress that one host flag and must not alter the other
    // postpass flag assignments nearby.
    const char *tailDisableEnvironment =
        "TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL";
    CHECK(harnessSource.find(tailDisableEnvironment) != std::string::npos);
    const size_t tailFlag = harnessSource.find("pp.slot0[2] |= 64u;");
    CHECK(tailFlag != std::string::npos);
    if (tailFlag != std::string::npos) {
        const size_t tailControlStart = harnessSource.rfind(
            "if (", tailFlag);
        CHECK(tailControlStart != std::string::npos);
        if (tailControlStart != std::string::npos) {
            const std::string tailControl = harnessSource.substr(
                tailControlStart, tailFlag - tailControlStart +
                                      std::strlen("pp.slot0[2] |= 64u;"));
            CHECK(tailControl.find(tailDisableEnvironment) !=
                  std::string::npos);
            CHECK(tailControl.find("!std::getenv") != std::string::npos);
            CHECK(tailControl.find("pp.slot0[2] |= 64u;") !=
                  std::string::npos);
            CHECK(tailControl.find("128u") == std::string::npos);
            CHECK(tailControl.find("32u") == std::string::npos);
        }
    }

    // The recovered-linear-output experiment is a final-store-only A/B. It
    // must be opt-in, must use a distinct postpass bit, must affect only SDR
    // output, and must leave the model-domain history write untouched.
    const char *linearOutputEnvironment =
        "TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT";
    CHECK(harnessSource.find(linearOutputEnvironment) != std::string::npos);
    const size_t linearOutputFlag = harnessSource.find(
        "pp.slot0[2] |= 256u;");
    CHECK(linearOutputFlag != std::string::npos);
    if (linearOutputFlag != std::string::npos) {
        const size_t linearControlStart = harnessSource.rfind(
            "if (", linearOutputFlag);
        CHECK(linearControlStart != std::string::npos);
        if (linearControlStart != std::string::npos) {
            const std::string linearControl = harnessSource.substr(
                linearControlStart, linearOutputFlag - linearControlStart +
                                      std::strlen("pp.slot0[2] |= 256u;"));
            CHECK(linearControl.find(linearOutputEnvironment) !=
                  std::string::npos);
            CHECK(linearControl.find("256u") != std::string::npos);
            CHECK(linearControl.find("64u") == std::string::npos);
            CHECK(linearControl.find("128u") == std::string::npos);
        }
    }
    CHECK(shader.find("const bool hdrOutput = slot2.w != 0u") !=
          std::string::npos);
    CHECK(shader.find("(slot0.z & 256u) != 0u") != std::string::npos);
    CHECK(shader.find("srgbToLinear(finalColor)") != std::string::npos);
    CHECK(shader.find("imageStore(u_historyOut, coord, vec4(modelColor, 1.0))") !=
          std::string::npos);

    // The motion-aware display-base candidate is opt-in only. It must sample
    // the source-resolution motion image through the same source coordinate
    // used by the postpass, then raise display-base weight only where motion
    // evidence is present; absent plumbing must leave the default branch
    // unchanged.
    const char *motionAwareDisplayEnvironment =
        "TFORGE_FSR4_MOTION_AWARE_DISPLAY_BASE";
    CHECK(harnessSource.find(motionAwareDisplayEnvironment) !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 1073741824u;") !=
          std::string::npos);
    CHECK(shader.find(
              "TFORGE_POSTPASS_MOTION_AWARE_DISPLAY_BASE = 1073741824u") !=
          std::string::npos);
    CHECK(shader.find("imageLoad(u_motion") != std::string::npos);
    CHECK(shader.find("sourceMotionPos") != std::string::npos);
    // The motion image is model-sized while currentBaseSourcePos is in source
    // pixels. The opt-in branch must convert the lookup coordinate before
    // reading u_motion, otherwise larger upscale ratios clamp most samples to
    // the motion texture edge and feed the display-base experiment bad motion.
    CHECK(shader.find("sourceToMotionScale") != std::string::npos);
    CHECK(shader.find("currentBaseSourcePos * sourceToMotionScale") !=
          std::string::npos);
    CHECK(shader.find("smoothstep(0.5, 4.0") != std::string::npos);
    CHECK(shader.find("motionAwareDisplayBase") != std::string::npos);

    // Display-space base resolution is an isolated Quality Lab probe. It
    // must be opt-in and select the separately uploaded display RGB tile for
    // both experimental and current composition. The current mode is the
    // normal best-findings base path; ignoring this request there makes a
    // valid color-space comparison silently render as model space.
    CHECK(harnessSource.find("TFORGE_FSR4_QUALITY_LAB_DISPLAY_BASE") !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 512u;") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_DISPLAY_SPACE_BASE = 512u") !=
          std::string::npos);
    CHECK(shader.find("currentUseDisplayBase") != std::string::npos);
    CHECK(shader.find("sampleDisplaySourceBicubic(currentBaseSourcePos") !=
          std::string::npos);
    CHECK(shader.find("sampleDisplaySourceBicubic(baseSourcePos") !=
          std::string::npos);

    // Edge-adaptive learned blending is another isolated A/B. It must derive
    // an edge signal from the existing source tile, reduce learned weight only
    // near strong source edges, and remain unreachable unless its explicit
    // environment switch sets a distinct postpass flag.
    const char *edgeAdaptiveEnvironment =
        "TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED";
    CHECK(harnessSource.find(edgeAdaptiveEnvironment) != std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 2147483648u;") !=
          std::string::npos);
    CHECK(shader.find(
              "TFORGE_POSTPASS_EDGE_ADAPTIVE_LEARNED = 2147483648u") !=
          std::string::npos);
    CHECK(shader.find("edgeAdaptiveLearned") != std::string::npos);
    CHECK(shader.find("edgeStrength") != std::string::npos);
    CHECK(shader.find("smoothstep(0.02, 0.18") != std::string::npos);
    CHECK(shader.find("adaptiveLearnedBlend") != std::string::npos);
    CHECK(harnessSource.find(
              "TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED_STRENGTH") !=
          std::string::npos);
    CHECK(harnessSource.find("edgeAdaptiveStrength") != std::string::npos);
    CHECK(shader.find("edgeAdaptiveStrength") != std::string::npos);

    // The archived FSR reference composes the learned reconstruction with the
    // reprojected history using the decoder's sigmoid blend value. The
    // single-history bypass is diagnostic-only: best-findings mode must not
    // disable the temporal history that its motion inputs are meant to drive.
    const char *singleHistoryEnvironment =
        "TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND";
    CHECK(harnessSource.find(singleHistoryEnvironment) != std::string::npos);
    // The host may set the bypass bit only when the explicit single-history
    // diagnostic is requested. A best-findings default would silently disable
    // the reference temporal blend in every normal dispatch.
    const std::string singleHistoryHostFlag = "pp.slot0[2] |= 1024u;";
    CHECK(harnessSource.find(singleHistoryHostFlag) != std::string::npos);
    const size_t singleHistoryFlagPos = harnessSource.find(singleHistoryHostFlag);
    CHECK(singleHistoryFlagPos != std::string::npos &&
          harnessSource.rfind(singleHistoryEnvironment, singleHistoryFlagPos) !=
              std::string::npos);
    const size_t singleHistoryCondition = harnessSource.rfind(
        "const bool singleHistoryResolve", singleHistoryFlagPos);
    CHECK(singleHistoryCondition != std::string::npos);
    if (singleHistoryCondition != std::string::npos) {
        const std::string condition = harnessSource.substr(
            singleHistoryCondition, singleHistoryFlagPos - singleHistoryCondition);
        CHECK(condition.find("bestFindingsTemporal") == std::string::npos);
        CHECK(condition.find(singleHistoryEnvironment) != std::string::npos);
    }
    const std::string singleHistoryFlag = "(slot0.z & 1024u) != 0u";
    CHECK(shader.find(singleHistoryFlag) != std::string::npos);
    const size_t firstSingleHistoryFlag = shader.find(singleHistoryFlag);
    CHECK(shader.find(singleHistoryFlag, firstSingleHistoryFlag + 1) !=
          std::string::npos);

    // Legacy regression probes are opt-in only. They let the campaign compare
    // the dirty-tree defaults against the prior round-anchor and recurrent
    // bias behavior without silently changing normal playback.
    CHECK(harnessSource.find(
              "TFORGE_FSR4_EXPERIMENTAL_LEGACY_ROUND") != std::string::npos);
    CHECK(harnessSource.find(
              "TFORGE_FSR4_EXPERIMENTAL_LEGACY_RECURRENT_BIAS") !=
          std::string::npos);
    CHECK(shader.find("slot0.z & 2048u") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_RECURRENT_BIAS0") !=
          std::string::npos);

    // The remaining postpass candidates must use only the existing constant
    // fields and the two already identified tail groups. They are all opt-in:
    // absent environment variables leave the current shader branches and
    // host values untouched.
    for (const char *environment : {
             "TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_RADIUS",
             "TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_SIGMA",
             "TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_WIDE_EXPONENT",
             "TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_LEGACY_NORMALIZATION",
             "TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_RAW_NORMALIZATION",
             "TFORGE_FSR4_EXPERIMENTAL_POSTPASS_CURRENT_WEIGHT",
             "TFORGE_FSR4_EXPERIMENTAL_POSTPASS_SWAP_TAIL_MAPPING",
             "TFORGE_FSR4_EXPERIMENTAL_POSTPASS_REVERSE_TAIL_CHANNELS"}) {
        CHECK(harnessSource.find(environment) != std::string::npos);
    }
    CHECK(shader.find("TFORGE_POSTPASS_CUSTOM_KERNEL") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_WIDE_EXPONENT") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_LEGACY_NORMALIZATION") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_CURRENT_WEIGHT") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_SWAP_TAIL_MAPPING") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_REVERSE_TAIL_CHANNELS") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_CUSTOM_KERNEL = 4194304u") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_CURRENT_WEIGHT = 33554432u") !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 4194304u;") !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 33554432u;") !=
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_RECURRENT_RESET_ONLY") !=
          std::string::npos);
    CHECK(shader.find("const float kernelRadius = customKernel") !=
          std::string::npos);
    CHECK(shader.find("const float kernelSigma = customKernel") !=
          std::string::npos);
    CHECK(shader.find("mix(temporalModelColor, current.rgb") !=
          std::string::npos);
    CHECK(shader.find("outputBiasBase") != std::string::npos);
    CHECK(shader.find("recurrentBiasBase") != std::string::npos);
    CHECK(shader.find("loadPostpassTailGroup(outputBiasBase") !=
          std::string::npos);
    CHECK(shader.find("loadPostpassTailGroup(recurrentBiasBase") !=
          std::string::npos);
    CHECK(shader.find("max(totalWeight, normalizationFloor)") !=
          std::string::npos);
    // The reference decoder applies the inverse Mu-law exponential to the
    // signed model output, then clamps the decoded linear result. Clamping
    // modelColor before exp() destroys negative undershoot information and
    // changes the official postpass response around edges and dark detail.
    const std::string removeMuLawBody =
        extractFunctionBody(shader, "vec3 removeMuLaw(vec3 modelColor)");
    CHECK(!removeMuLawBody.empty());
    CHECK(removeMuLawBody.find("exp(8.51788 * modelColor)") !=
          std::string::npos);
    CHECK(removeMuLawBody.find("exp(8.51788 * max(modelColor") ==
          std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_LEGACY_PRECLAMP_MULAW") ==
          std::string::npos);
    CHECK(harnessSource.find(
              "TFORGE_FSR4_EXPERIMENTAL_LEGACY_PRECLAMP_MULAW") ==
          std::string::npos);
    CHECK(harnessSource.find("readExperimentalFloat") !=
          std::string::npos);
    // CAS is a postpass over the selected composition. It must consume the
    // incoming color; otherwise it silently replaces every experimental
    // composition with an unrelated source-only resolve.
    const std::string casBody = extractFunctionBody(
        shader, "vec3 applyCas(vec3 color,");
    CHECK(!casBody.empty());
    CHECK(casBody.find("srgbToLinear(color)") != std::string::npos);

    // Spatial controls must use the same model-space source contract as the
    // known-good path. Decode that representation exactly once at the point
    // where a spatial result becomes display RGB; filtering the separately
    // encoded presentation copy changes chroma/detail response and regresses
    // the baseline against Lanczos.
    CHECK(shader.find("binding = 10, rgba8) readonly uniform image2D u_sourceDisplay") !=
          std::string::npos);
    // Reduced-model display sampling must use the actual display image
    // dimensions. The old shared display tile used model-space bounds and
    // could read the wrong pixels when source and model resolutions differed.
    CHECK(shader.find("const ivec2 displaySize = imageSize(u_sourceDisplay)") !=
          std::string::npos);
    CHECK(shader.find("const ivec2 modelSize = imageSize(u_sourceColor)") !=
          std::string::npos);
    CHECK(shader.find("const vec2 displayScale = vec2(displaySize) /") !=
          std::string::npos);
    CHECK(shader.find("imageLoad(u_sourceDisplay, ivec2(sx, sy))") !=
          std::string::npos);
    for (const char *signature : {"vec3 sampleSourceBicubic(",
                                  "vec3 sampleSourceBilinear(",
                                  "vec3 sampleSourceCubicBC(",
                                  "vec3 sampleSourceLanczos2("}) {
        const std::string baseBody = extractFunctionBody(shader, signature);
        CHECK(!baseBody.empty());
        CHECK(baseBody.find("loadSourceCached") != std::string::npos);
        CHECK(baseBody.find("loadDisplaySourceCached") == std::string::npos);
    }

    // The current default anchors the learned footprint at floor(sourcePos).
    // The legacy round() anchor is retained only behind the explicit
    // regression-probe bit, so both alternatives must be visibly guarded.
    CHECK(shader.find("floor(sourcePos)") != std::string::npos);
    CHECK(shader.find("round(sourcePos)") != std::string::npos);
    CHECK(shader.find("slot0.z & 2048u") != std::string::npos);

    // A learned blend may need the residual, tone, sharpen, and CAS stages
    // together. The residual stage must therefore be explicitly stackable
    // with direct_blend; otherwise a weak learned candidate can be rejected
    // only because its stabilizing/detail stage was unavailable.
    CHECK(shader.find("const bool stackedResidual = slot3.x == 4u ||") !=
          std::string::npos);
    CHECK(shader.find("slot3.x == 3u && slot5.y > 0.0") !=
          std::string::npos);

    // The motion-aware residual candidate is capture-only. It must use the
    // existing per-frame reactive signal, reduce learned residual strength
    // only when explicitly enabled, and leave the default detail-residual
    // branch unchanged when the environment switch is absent.
    const char *motionAwareResidualEnvironment =
        "TFORGE_FSR4_MOTION_AWARE_RESIDUAL";
    CHECK(harnessSource.find(motionAwareResidualEnvironment) !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot0[2] |= 16777216u;") !=
          std::string::npos);
    CHECK(shader.find(
              "TFORGE_POSTPASS_MOTION_AWARE_RESIDUAL = 2097152u") !=
          std::string::npos);
  CHECK(shader.find("motionAwareResidual") != std::string::npos);
  CHECK(shader.find("slot6.x") != std::string::npos);
  CHECK(shader.find("smoothstep(0.005, 0.025") != std::string::npos);
  CHECK(shader.find("reactiveAverage") == std::string::npos);

    // When Quality Lab is enabled in the normal current composition, its
    // selected base filter must reach the current base resolve. Disabled lab
    // mode must retain the legacy environment-controlled bilinear/Catmull
    // branch so ordinary playback is unchanged.
    CHECK(shader.find("const bool useQualityCurrentBase = qualityEnabled &&") !=
          std::string::npos);
    CHECK(shader.find("currentUseDisplayBase") != std::string::npos);
    CHECK(shader.find("? sampleDisplaySourceBicubic(currentBaseSourcePos") !=
          std::string::npos);
    CHECK(shader.find("? removeMuLaw(sampleSourceBase(") !=
          std::string::npos);

    // The confidence blend is an opt-in control over the history policy. Zero
    // keeps the confidence gate, one reproduces the explicit ungated path,
    // and intermediate values interpolate the learned contribution without
    // changing the decoder, history format, or default behavior.
    const char *confidenceBlendEnvironment =
        "TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND";
    CHECK(harnessSource.find(confidenceBlendEnvironment) !=
          std::string::npos);
    CHECK(harnessSource.find(
              "std::clamp(std::strtof(value, nullptr), 0.0f, 1.0f)") !=
          std::string::npos);
    CHECK(harnessSource.find("1.0f - confidenceBlend") !=
          std::string::npos);
    CHECK(harnessSource.find("confidenceBlend +") !=
          std::string::npos);

    // Experimental composition normally preserves the historical ungated
    // quality-lab path.  A separate opt-in must be available for experiments
    // that want learned contribution to respect codec/history confidence;
    // this keeps the product path unchanged while making that comparison
    // independently testable.
    CHECK(harnessSource.find(
              "TFORGE_FSR4_ENABLE_EXPERIMENTAL_CONFIDENCE_GATE") !=
          std::string::npos);
    CHECK(harnessSource.find(
              "experimentalComposition && !enableExperimentalConfidenceGate") !=
          std::string::npos);
    CHECK(harnessSource.find("pp.slot5[0] = experimentalComposition") !=
          std::string::npos);
    CHECK(harnessSource.find("learnedStrength * effectiveConfidence") !=
          std::string::npos);

    const std::string harnessHeader =
        readSourceFile("src/render/Fsr4DispatchHarness.hpp");
    // Spatial controls must be able to compare a learned branch against the
    // same unjittered base resolve used by base_only.  This is opt-in because
    // it changes only diagnostic composition; the host flag and shader bit
    // must remain explicit so normal playback cannot change accidentally.
    CHECK(shader.find("experimentalUnjitteredBase") != std::string::npos);
    CHECK(harnessSource.find("TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED") !=
          std::string::npos);
    const std::string uploaderHeader =
        readSourceFile("src/render/GpuImageUploader.hpp");
    const std::string playbackSource =
        readSourceFile("src/core/PlaybackEngine.cpp");
    CHECK(harnessHeader.find("sourceDisplayView") != std::string::npos);
    CHECK(harnessHeader.find("sourceDisplayImage") != std::string::npos);
    CHECK(harnessSource.find("std::array<VkDescriptorSetLayoutBinding, 11>") !=
          std::string::npos);
    CHECK(harnessSource.find("in.sourceDisplayView") != std::string::npos);
    CHECK(uploaderHeader.find("rawPresentationView()") != std::string::npos);
    CHECK(playbackSource.find(
              "in.sourceDisplayView = firstUploader->rawPresentationView()") !=
          std::string::npos);

    if (g_failures == 0) {
        std::printf("fsr4_postpass_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_postpass_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
