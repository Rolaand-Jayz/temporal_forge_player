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
    const std::string expand = readSource("shaders/fsr4/codec_motion_expand.comp");
    const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");

    CHECK(decoder.find("m.source < 0 ? -1 : (m.source > 0 ? 1 : 0)") !=
          std::string::npos);
    CHECK(playback.find("mv.source <= 0") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvX)") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvY)") != std::string::npos);
    CHECK(playback.find("mv.mvX *= sx") != std::string::npos);
    CHECK(playback.find("mv.mvY *= sy") != std::string::npos);
    CHECK(playback.find("pastReferenceMotion(fsrDf.motionVectors)") !=
          std::string::npos);

    CHECK(expand.find("atomicMax(owners[pixelIndex], vectorIndex + 1u)") !=
          std::string::npos);
    CHECK(expand.find("vectors[owner - 1u].motion") != std::string::npos);
    CHECK(expand.find("imageStore(motionImage") != std::string::npos);

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
    CHECK(harness.find("pp.slot0[2] = in.reset ? 1u : 0u") !=
          std::string::npos);

    if (g_failures == 0) {
        std::printf("fsr4_motion_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_motion_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
