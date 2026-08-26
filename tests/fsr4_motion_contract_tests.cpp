// fsr4_motion_contract_tests.cpp — M3 tests for causal codec motion.
//
// These tests lock the current motion contract at its boundaries: FFmpeg
// source direction, CPU causal filtering/scaling, GPU sparse expansion, and
// confidence/reset handoff into temporal dispatch.
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

int main() {
    const std::string decoder = readSource("src/media/VideoDecoder.cpp");
    const std::string playback = readSource("src/core/PlaybackEngine.cpp");
    const std::string sideSynth = readSource("src/render/SideBufferSynth.cpp");
    const std::string sideSynthHeader =
        readSource("src/render/SideBufferSynth.hpp");
    const std::string uploaderHeader =
        readSource("src/render/GpuImageUploader.hpp");
    const std::string uploader = readSource("src/render/GpuImageUploader.cpp");
    const std::string harnessHeader =
        readSource("src/render/Fsr4DispatchHarness.hpp");
    const std::string expand = readSource("shaders/fsr4/codec_motion_expand.comp");
    const std::string prepass = readSource("shaders/fsr4/prepass_pq_eotf.comp");
    const std::string postpass =
        readSource("shaders/fsr4/postpass_composite.comp");
    const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");

    CHECK(decoder.find("m.source < 0 ? -1 : (m.source > 0 ? 1 : 0)") !=
          std::string::npos);
    CHECK(playback.find("mv.source <= 0") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvX)") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvY)") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS") !=
          std::string::npos);
    CHECK(playback.find("motionLimitMultiplier") != std::string::npos);
    CHECK(playback.find("mv.mvX *= sx") != std::string::npos);
    CHECK(playback.find("mv.mvY *= sy") != std::string::npos);
    CHECK(playback.find("scaleMotionToModel(\n            pastMotion") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION") !=
          std::string::npos);
    CHECK(playback.find("refineCodecMotion") != std::string::npos);
    CHECK(sideSynthHeader.find("refineCodecMotion") != std::string::npos);
    CHECK(sideSynth.find("localSad") != std::string::npos);
    CHECK(sideSynth.find("refinementRadius") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE") !=
          std::string::npos);
    CHECK(playback.find("frameIndex == 0 ? std::vector<MvEntry>{}") !=
          std::string::npos);

    CHECK(expand.find("atomicMax(owners[pixelIndex], vectorIndex + 1u)") !=
          std::string::npos);
    CHECK(expand.find("vectors[owner - 1u].motion") != std::string::npos);
    CHECK(expand.find("imageStore(motionImage") != std::string::npos);
    CHECK(expand.find("layout(binding = 3, r8) writeonly uniform image2D validityImage") !=
          std::string::npos);
    CHECK(expand.find("owner == 0u ? 0.0 : 1.0") != std::string::npos);
    CHECK(prepass.find("binding = 6, r8") != std::string::npos);
    CHECK(prepass.find("u_motionValidity") != std::string::npos);
    CHECK(prepass.find("motionCovered") != std::string::npos);
    CHECK(prepass.find("if (motionCovered && onscreen)") != std::string::npos);
    CHECK(prepass.find("if (motionCovered)") != std::string::npos);
    CHECK(prepass.find("slot0.z & 512u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 8192u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 16384u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 32768u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 65536u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 262144u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 524288u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 1048576u") != std::string::npos);
    CHECK(prepass.find("sampleHistoryLinear") != std::string::npos);
    CHECK(prepass.find("sampleHistoryNearest") != std::string::npos);
    CHECK(postpass.find("TFORGE_POSTPASS_RECURRENT_RESET_ONLY") !=
          std::string::npos);
    CHECK(postpass.find("imageStore(u_recurrentOut, coord, vec4(0.0))") !=
          std::string::npos);
    CHECK(uploaderHeader.find("motionValidityView") != std::string::npos);
    CHECK(uploader.find("VK_FORMAT_R8_UNORM") != std::string::npos);
    CHECK(uploader.find("motionValidity_") != std::string::npos);
    CHECK(harnessHeader.find("motionValidityView") != std::string::npos);
    CHECK(harness.find("in.motionValidityView") != std::string::npos);
    CHECK(harness.find("in.motionValidityImage") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_DISABLE_MOTION_VALIDITY") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY") !=
          std::string::npos);

    CHECK(harness.find("in.historyConfidence") != std::string::npos);
    CHECK(harness.find("in.reset ? 1u : 0u") != std::string::npos);
    CHECK(playback.find("historyConfidence = sideInputs.motionConfidence") !=
          std::string::npos);

    // A detected scene cut must take the reset-safe upload path and reach the
    // postpass reset bit. Upstream: SideBufferSynth/PlaybackEngine reset
    // detection. Downstream: temporal history invalidation in postpass; a
    // regression here can leave stale native-path history after a cut.
    CHECK(playback.find("const bool initializeNeutral =") !=
          std::string::npos);
    CHECK(harness.find("pp.slot0[2] = recurrentResetOnly ? 268435456u") !=
          std::string::npos);

    if (g_failures == 0) {
        std::printf("fsr4_motion_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_motion_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
