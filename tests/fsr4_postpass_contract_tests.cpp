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
    CHECK(!shader.empty());
    const size_t declaration = shader.find("float weightParams[]");
    CHECK(declaration != std::string::npos);
    CHECK(shader.find("weightParams[", declaration + 1) != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_PARAM_OFFSET") != std::string::npos);
    CHECK(shader.find("postpassParameterTrace") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_OUTPUT_BIAS0") != std::string::npos);
    CHECK(shader.find("TFORGE_POSTPASS_OUTPUT_BIAS1") != std::string::npos);
    CHECK(shader.find("slot0.z & 64u") != std::string::npos);

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
    CHECK(shader.find("shared vec4 displaySourceTile") != std::string::npos);
    for (const char *signature : {"vec3 sampleSourceBicubic(",
                                  "vec3 sampleSourceBilinear(",
                                  "vec3 sampleSourceCubicBC(",
                                  "vec3 sampleSourceLanczos2("}) {
        const std::string baseBody = extractFunctionBody(shader, signature);
        CHECK(!baseBody.empty());
        CHECK(baseBody.find("loadSourceCached") != std::string::npos);
        CHECK(baseBody.find("loadDisplaySourceCached") == std::string::npos);
    }

    const std::string harnessHeader =
        readSourceFile("src/render/Fsr4DispatchHarness.hpp");
    const std::string harnessSource =
        readSourceFile("src/render/Fsr4DispatchHarness.cpp");
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
