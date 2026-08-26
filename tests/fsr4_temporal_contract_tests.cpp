// fsr4_temporal_contract_tests.cpp — M2 tests written before implementation.
//
// These tests pin the resource-graph contract before history formats or
// reprojection ownership change. They are source-level because the existing
// live GPU harness test is intentionally disabled on this checkout; compiled
// shader/runtime evidence remains a later part of the M2 gate.
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
    const std::string uploader = readSource("src/render/GpuImageUploader.cpp");
    const std::string prepass = readSource("shaders/fsr4/prepass_pq_eotf.comp");
    const std::string postpass = readSource("shaders/fsr4/postpass_composite.comp");
    const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");

    CHECK(!uploader.empty());
    CHECK(!prepass.empty());
    CHECK(!postpass.empty());
    CHECK(!harness.empty());

    // Model history must preserve FP16 range; RGB10/A2 silently clamps state.
    CHECK(uploader.find("VK_FORMAT_R16G16B16A16_SFLOAT, outputUsage") !=
          std::string::npos);
    CHECK(uploader.find("fsr4_history_a") != std::string::npos);
    CHECK(uploader.find("fsr4_history_b") != std::string::npos);

    // Both stages consume the same target-grid reprojected-color contract.
    CHECK(prepass.find("u_reprojectedColor") != std::string::npos);
    CHECK(postpass.find("u_reprojectedColor") != std::string::npos);
    CHECK(harness.find("reprojectedColor") != std::string::npos);

    // The postpass must not independently derive a second history reprojection.
    CHECK(postpass.find("sampleHistoryBicubic") == std::string::npos);
    CHECK(postpass.find("imageLoad(u_reprojectedColor") != std::string::npos);

    // When history feedback is unavailable, the shared reprojection resource
    // must fall back to the complete current resolve. Attenuating that color
    // by the temporal confidence weight makes the postpass blend toward a
    // dark image, because the postpass consumes this resource as color rather
    // than as a weighted feature contribution.
    CHECK(prepass.find("historyModel = current.rgb;") != std::string::npos);
    CHECK(prepass.find("historyModel = temporalSampleWeight * current.rgb;") ==
          std::string::npos);

    // The current composition may blend a jittered learned reconstruction,
    // but its non-temporal base component must remain on the stable source
    // grid. At severe upscale ratios the learned confidence can approach
    // zero; jittering the dominant base then creates a large spatial loss
    // without providing temporal accumulation to recover it.
    CHECK(postpass.find("sourcePos - slot1.xy") != std::string::npos);
    // The recovered jittered-base phase is diagnostic-only; normal playback
    // must continue to select the stable coordinate unless explicitly asked.
    CHECK(postpass.find("(slot0.z & 32u) != 0u") != std::string::npos);
    CHECK(postpass.find("sampleSourceBicubic(currentBaseSourcePos") !=
          std::string::npos);

    // The shared postpass tile is populated from jittered coordinates but is
    // also sampled by the stable base coordinate. Reserve one extra source
    // texel on every side so the full cubic footprint remains inside shared
    // memory for any legal half-pixel jitter phase.
    CHECK(postpass.find("floor(groupSourcePos)) - ivec2(3)") !=
          std::string::npos);
    CHECK(postpass.find("ceil(groupLastSourcePos)) + ivec2(2)") !=
          std::string::npos);

    // History, reprojected color, and recurrent state must all be FP16 model
    // resources, while display output remains a separate presentation image.
    CHECK(prepass.find("rgba16f) readonly uniform image2D u_history") !=
          std::string::npos);
    CHECK(postpass.find("rgba16f) readonly uniform image2D u_history") !=
          std::string::npos);
    CHECK(postpass.find("rgba16f) writeonly uniform image2D u_historyOut") !=
          std::string::npos);
    CHECK(postpass.find("rgba16f) readonly uniform image2D u_reprojectedColor") !=
          std::string::npos);

    // Reset and ping-pong ownership remain explicit rather than inferred from
    // descriptor reuse.
    CHECK(uploader.find("historyIndex_.store(0") != std::string::npos);
    CHECK(harness.find("in.reset") != std::string::npos);
    CHECK(harness.find("historyReadImage") != std::string::npos);
    CHECK(harness.find("historyWriteImage") != std::string::npos);

    // The normal path must retain an unbounded completion wait. Diagnostic
    // capture mode may opt into a finite wait so a hung GPU submission becomes
    // a classified failed row instead of consuming the whole campaign.
    CHECK(harness.find("TFORGE_FSR4_FENCE_TIMEOUT_MS") != std::string::npos);
    CHECK(harness.find("VK_TIMEOUT") != std::string::npos);
    CHECK(harness.find("temporal fence timeout") != std::string::npos);

    // Opt-in first-dispatch tracing must identify the last completed GPU stage
    // without changing the default render path or image-quality behavior.
    CHECK(harness.find("TFORGE_FSR4_DISPATCH_TRACE") != std::string::npos);
    // The prepass input can be sampled only through an explicit diagnostic;
    // normal playback must never pay for this readback.
    CHECK(harness.find("TFORGE_FSR4_DUMP_PREPASS_INPUT") !=
          std::string::npos);
    CHECK(harness.find("dispatch trace: prepass {}") != std::string::npos);
    CHECK(harness.find("prepassDisabled") != std::string::npos);
    CHECK(harness.find("dispatch trace: native graph complete") != std::string::npos);
    CHECK(harness.find("dispatch trace: queue submitted") != std::string::npos);

    // The motion-aware learned-strength candidate must consume the same
    // reactive signal already synthesized for the per-frame side buffers.
    // It is opt-in so the current playback contract remains unchanged until
    // matched real-scene captures prove that reducing learned history during
    // active changes improves temporal stability.
    CHECK(harness.find("reactiveAverage") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_ADAPTIVE_LEARNED_STRENGTH") !=
          std::string::npos);
    CHECK(harness.find("confidenceGate") != std::string::npos);

    // Display-color history has a separate opt-in confidence threshold. The
    // learned-strength gate alone cannot stop a bad codec vector from
    // pulling stale color into the prepass history image.
    CHECK(harness.find("TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD") !=
          std::string::npos);
    CHECK(prepass.find("historyConfidenceThreshold") != std::string::npos);
    CHECK(prepass.find("HISTORY_CONFIDENCE_GATE") != std::string::npos);

    if (g_failures == 0) {
        std::printf("fsr4_temporal_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_temporal_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
