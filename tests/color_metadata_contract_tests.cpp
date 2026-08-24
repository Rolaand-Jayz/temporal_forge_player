// color_metadata_contract_tests.cpp — M4 tests for decode-to-upload color truth.
//
// These are source-boundary tests because the live Vulkan color path is not
// enabled in the ordinary headless test suite. They deliberately fail until
// the decoder preserves metadata and both YUV shaders consume the same
// metadata-driven chroma phase. The assertions are the contract; production
// code must change to satisfy them rather than weakening these checks.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

    if (g_failures == 0) {
        std::printf("color_metadata_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "color_metadata_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
