// color_metadata_contract_tests.cpp — M4 tests for decode-to-upload color truth.
//
// These are source-boundary tests because the live Vulkan color path is not
// enabled in the ordinary headless test suite. They deliberately fail until
// the decoder preserves metadata and both YUV shaders consume the same
// metadata-driven chroma phase. The assertions are the contract; production
// code must change to satisfy them rather than weakening these checks.
#include <cstdio>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "render/upload/YuvConstants.hpp"

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static std::string readSource(const char *relative) {
#ifdef TFORGE_SOURCE_ROOT
    const std::filesystem::path path =
        std::filesystem::path(TFORGE_SOURCE_ROOT) / relative;
#else
    const std::filesystem::path path = relative;
#endif
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static void checkMetadataContract(const std::string &header,
                                  const std::string &decoder) {
    CHECK(header.find("colorPrimaries") != std::string::npos);
    CHECK(header.find("chromaLocation") != std::string::npos);
    CHECK(header.find("bitDepth") != std::string::npos);
    CHECK(decoder.find("sourceFrame->color_primaries") != std::string::npos);
    CHECK(decoder.find("sourceFrame->chroma_location") != std::string::npos);
    CHECK(decoder.find("av_pix_fmt_desc_get") != std::string::npos);
    CHECK(decoder.find("out.bitDepth") != std::string::npos);
    CHECK(decoder.find("out.colorPrimaries") != std::string::npos);
    CHECK(decoder.find("out.chromaLocation") != std::string::npos);
}

static void checkConversionContract(const std::string &constants,
                                    const std::string &yuv,
                                    const std::string &drm) {
    CHECK(constants.find("chromaPhase") != std::string::npos);
    CHECK(constants.find("frame.chromaLocation") != std::string::npos);
    CHECK(constants.find("frame.bitDepth") != std::string::npos);
    CHECK(yuv.find("pc.chroma") != std::string::npos);
    CHECK(drm.find("pc.chroma") != std::string::npos);
    CHECK(yuv.find("chromaPhase") != std::string::npos);
    CHECK(drm.find("chromaPhase") != std::string::npos);
    CHECK(yuv.find("vec2(0.5) - vec2(0.5)") == std::string::npos);
    CHECK(drm.find("vec2(0.5) - vec2(0.5)") == std::string::npos);

    // Centered 4:2:0 chroma places the first luma pixel at UV -0.25. A
    // four-tap cubic footprint therefore reaches two chroma texels before
    // each 16x16 luma workgroup. Both upload paths need the same 12x12 cache
    // with a two-texel negative halo; an 11x11/-1 cache reads outside shared
    // memory and produces periodic color bands at workgroup boundaries.
    for (const std::string *shader : {&yuv, &drm}) {
        CHECK(shader->find("const int UV_TILE_WIDTH = 12") !=
              std::string::npos);
        CHECK(shader->find("groupCoord / 2 - ivec2(2)") !=
              std::string::npos);
        CHECK(shader->find("const int UV_TILE_WIDTH = 11") ==
              std::string::npos);
    }
}

static void checkBitDepthContract(const std::string &uploader) {
    CHECK(uploader.find("frame.bitDepth > 8") != std::string::npos);
    CHECK(uploader.find("VK_FORMAT_R16_UNORM") != std::string::npos);
    CHECK(uploader.find("VK_FORMAT_R16G16_UNORM") != std::string::npos);
    CHECK(uploader.find("bytesPerSample") != std::string::npos);
}

static void checkRec709InputEotfContract(const std::string &constants,
                                         const std::string &yuv,
                                         const std::string &drm) {
    // The opt-in transfer-function experiment is carried in the existing
    // reserved push-constant component. Both upload paths must decode the
    // same flag and use the exact Rec.709 inverse OETF constants while the
    // raw presentation image continues to receive the original RGB values.
    CHECK(constants.find(
              "TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF") !=
          std::string::npos);
    CHECK(constants.find("YUV_FLAG_REC709_INPUT_EOTF") !=
          std::string::npos);
    CHECK(constants.find("std::bit_cast<float>") != std::string::npos);

    for (const std::string *shader : {&yuv, &drm}) {
        CHECK(shader->find("YUV_FLAG_REC709_INPUT_EOTF") !=
              std::string::npos);
        CHECK(shader->find("floatBitsToUint(pc.chroma.w)") !=
              std::string::npos);
        CHECK(shader->find("0.081") != std::string::npos);
        CHECK(shader->find("encoded / 4.5") != std::string::npos);
        CHECK(shader->find("encoded + 0.099") != std::string::npos);
        CHECK(shader->find("1.0 / 0.45") != std::string::npos);
    }
    // The two shaders use different binding names for the same raw output;
    // verify each exact store independently so the Rec.709 branch cannot
    // accidentally alter the presentation image.
    CHECK(yuv.find("imageStore(u_raw, p, vec4(rgb, 1.0))") !=
          std::string::npos);
    CHECK(drm.find("imageStore(rawImg, p, vec4(rgb, 1.0))") !=
          std::string::npos);
}

static void checkUnknownMatrixBt709Contract(const std::string &constants) {
    // This experiment is deliberately narrower than the transfer-function
    // experiment above: it changes only the fallback for unspecified matrix
    // metadata. The runtime checks below prove the default remains BT.601 for
    // unknown sub-720p input and that explicit matrix metadata is invariant.
    CHECK(constants.find(
              "TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709") !=
          std::string::npos);
    CHECK(constants.find("AVCOL_SPC_UNSPECIFIED") != std::string::npos);
    CHECK(constants.find("frame.height < 720") != std::string::npos);

    temporal_forge::DecodedVideoFrame frame;
    frame.width = 640;
    frame.height = 360;
    frame.colorRange = AVCOL_RANGE_MPEG;
    frame.colorSpace = AVCOL_SPC_UNSPECIFIED;

    unsetenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709");
    const auto legacy = temporal_forge::yuvPushConstants(frame, false, 0.0f);
    setenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709", "1", 1);
    const auto overridden =
        temporal_forge::yuvPushConstants(frame, false, 0.0f);
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709");

    // Limited-range BT.601 and BT.709 have distinct R/V coefficients. These
    // exact values make the test fail if the environment is ignored or if it
    // leaks into any path other than the unspecified low-resolution fallback.
    const float limitedChromaScale = 255.0f / 224.0f;
    const float expectedBt601Rv = 2.0f * (1.0f - 0.2990f) * limitedChromaScale;
    const float expectedBt709Rv = 2.0f * (1.0f - 0.2126f) * limitedChromaScale;
    CHECK(std::fabs(legacy.rV - expectedBt601Rv) < 1e-5f);
    CHECK(std::fabs(overridden.rV - expectedBt709Rv) < 1e-5f);
    CHECK(std::fabs(legacy.rV - overridden.rV) > 1e-3f);

    // Explicit matrix metadata must remain coefficient-equivalent with the
    // experiment unset and set, including explicit BT.601, BT.709, and BT.2020.
    for (const int matrix : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709,
                             AVCOL_SPC_BT2020_NCL}) {
        frame.colorSpace = matrix;
        unsetenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709");
        const auto explicitDefault =
            temporal_forge::yuvPushConstants(frame, false, 0.0f);
        setenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709", "1", 1);
        const auto explicitOverride =
            temporal_forge::yuvPushConstants(frame, false, 0.0f);
        CHECK(explicitDefault.rV == explicitOverride.rV);
        CHECK(explicitDefault.gU == explicitOverride.gU);
        CHECK(explicitDefault.gV == explicitOverride.gV);
        CHECK(explicitDefault.bU == explicitOverride.bU);
    }
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709");
}

static void checkOptInTransferAndChromaControls(const std::string &constants,
                                                const std::string &yuv,
                                                const std::string &drm) {
    CHECK(constants.find("TFORGE_FSR4_INPUT_TRANSFER") != std::string::npos);
    CHECK(constants.find("TFORGE_FSR4_INPUT_SHARPEN_STRENGTH") ==
          std::string::npos); // uploader owns sharpening, not color constants
    CHECK(constants.find("TFORGE_FSR4_CHROMA_FILTER") != std::string::npos);
    CHECK(constants.find("TFORGE_FSR4_CHROMA_PHASE") != std::string::npos);
    for (const std::string *shader : {&yuv, &drm}) {
        CHECK(shader->find("YUV_FLAG_LINEAR_INPUT_EOTF") != std::string::npos);
        CHECK(shader->find("YUV_FLAG_CHROMA_BILINEAR") != std::string::npos);
        CHECK(shader->find("loadUvBilinear") != std::string::npos);
        CHECK(shader->find("loadUvFiltered") != std::string::npos);
    }

    temporal_forge::DecodedVideoFrame frame;
    frame.width = 640;
    frame.height = 360;
    frame.colorRange = AVCOL_RANGE_MPEG;
    frame.colorSpace = AVCOL_SPC_BT709;

    unsetenv("TFORGE_FSR4_INPUT_TRANSFER");
    unsetenv("TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF");
    unsetenv("TFORGE_FSR4_CHROMA_FILTER");
    unsetenv("TFORGE_FSR4_CHROMA_PHASE");
    const auto defaults = temporal_forge::yuvPushConstants(frame, false, 0.3f);
    CHECK(std::bit_cast<uint32_t>(defaults.reserved) == 0u);
    CHECK(defaults.chromaPhaseX == 0.5f);
    CHECK(defaults.chromaPhaseY == 0.5f);

    setenv("TFORGE_FSR4_INPUT_TRANSFER", "linear", 1);
    setenv("TFORGE_FSR4_CHROMA_FILTER", "bilinear", 1);
    setenv("TFORGE_FSR4_CHROMA_PHASE", "top-left", 1);
    const auto optIn = temporal_forge::yuvPushConstants(frame, false, 0.3f);
    CHECK((std::bit_cast<uint32_t>(optIn.reserved) & 2u) != 0u);
    CHECK((std::bit_cast<uint32_t>(optIn.reserved) & 4u) != 0u);
    CHECK(optIn.chromaPhaseX == 0.25f);
    CHECK(optIn.chromaPhaseY == 0.25f);

    unsetenv("TFORGE_FSR4_INPUT_TRANSFER");
    unsetenv("TFORGE_FSR4_CHROMA_FILTER");
    unsetenv("TFORGE_FSR4_CHROMA_PHASE");
}

int main() {
    const std::string header = readSource("src/media/VideoDecoder.hpp");
    const std::string decoder = readSource("src/media/VideoDecoder.cpp");
    const std::string constants = readSource("src/render/upload/YuvConstants.cpp");
    const std::string uploader = readSource("src/render/GpuImageUploader.cpp");
    const std::string yuv = readSource("shaders/fsr4/yuv_to_fsr_input.comp");
    const std::string drm = readSource("shaders/fsr4/drm_yuv_to_fsr_input.comp");

    checkMetadataContract(header, decoder);
    checkConversionContract(constants, yuv, drm);
    checkBitDepthContract(uploader);
    checkRec709InputEotfContract(constants, yuv, drm);
    checkUnknownMatrixBt709Contract(constants);
    checkOptInTransferAndChromaControls(constants, yuv, drm);

    if (g_failures == 0) {
        std::printf("color_metadata_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "color_metadata_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
