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
    const std::string uploaderHeader = readSource("src/render/GpuImageUploader.hpp");
    const std::string prepass = readSource("shaders/fsr4/prepass_pq_eotf.comp");
    const std::string postpass = readSource("shaders/fsr4/postpass_composite.comp");
    const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");
    const std::string playback = readSource("src/core/PlaybackEngine.cpp");

    CHECK(!uploader.empty());
    CHECK(!prepass.empty());
    CHECK(!postpass.empty());
    CHECK(!harness.empty());
    CHECK(!playback.empty());

    // The FSR1/EASU pre-neural candidate must be an explicit, dimension-gated
    // opt-in. It must feed the FSR4 model color image through the existing
    // RGB10 conversion and bind the EASU display image as the model's source
    // display input, rather than merely running a disconnected diagnostic.
    CHECK(playback.find("TFORGE_FSR4_PRE_EASU") != std::string::npos);
    CHECK(playback.find("preEasu") != std::string::npos);
    CHECK(playback.find("firstUploader->dispatchEasu()") != std::string::npos);
    CHECK(playback.find("firstUploader->easuColorView()") != std::string::npos);
    CHECK(playback.find("downscaleRgb10(") != std::string::npos);
    // A dispatch log alone is not enough to prove the FSR1 candidate reached
    // the model. The opt-in handoff must expose compact readback fingerprints
    // for the EASU image and the post-downscale model image so an A/B capture
    // can distinguish an active prepass from a disconnected probe.
    CHECK(playback.find("pre-neural EASU image frame={}") != std::string::npos);
    CHECK(playback.find("pre-neural model input frame={}") != std::string::npos);
    CHECK(playback.find("fingerprint=0x{:016x}") != std::string::npos);
    CHECK(playback.find("model input fingerprint ") != std::string::npos);
    CHECK(playback.find("(ordinary path) frame={}") != std::string::npos);
    // EASU selection is an explicit opt-in independent of the neural model's
    // final dimensions. The prepass may be 2x native and then be reduced into
    // the model input for ratios such as 720p -> 1080p.
    CHECK(playback.find("pair.preEasu") != std::string::npos);
    CHECK(playback.find("jitterPair.preEasu") != std::string::npos);
    CHECK(playback.find("neuralTargetW >= static_cast<uint32_t>(df.width) * 2u") ==
          std::string::npos);
    CHECK(playback.find("neuralTargetH >= static_cast<uint32_t>(df.height) * 2u") ==
          std::string::npos);
    CHECK(playback.find("static_cast<uint32_t>(df.width) * 2u") !=
          std::string::npos);
    CHECK(playback.find("static_cast<uint32_t>(df.height) * 2u") !=
          std::string::npos);

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

    // A covered motion block is not sufficient history evidence when its
    // reprojected coordinate leaves the history image. The shader must sample
    // and blend history only for an on-screen correspondence; otherwise the
    // untouched zero value is blended as black history and creates dark trails
    // at motion boundaries. Upstream: motion coverage and historyPos.
    // Downstream: reprojectedColor, temporal features, and postpass output.
    const auto historyBlend = prepass.rfind("historyModel = max(");
    CHECK(historyBlend != std::string::npos);
    CHECK(historyBlend != std::string::npos &&
          prepass.rfind("if (motionCovered && onscreen)", historyBlend) !=
              std::string::npos);
    // A rejected or uncovered correspondence must remain distinguishable from
    // a valid static history sample. The recovered temporal contract uses a
    // zero history value for that per-pixel rejection; current-as-history is
    // reserved for a full reset or the explicit no-history compatibility path.
    const auto invalidHistoryBranch = prepass.find(
        "historyModel = (slot0.z & TFORGE_PREPASS_CURRENT_INVALID_HISTORY)");
    CHECK(invalidHistoryBranch != std::string::npos);
    CHECK(invalidHistoryBranch != std::string::npos &&
          prepass.find("vec3(0.0)", invalidHistoryBranch) !=
              std::string::npos);
    // The reference path publishes zero history when correspondence is
    // rejected. Current-as-history is the compatibility diagnostic and must
    // be explicitly enabled so disocclusions are visible to the network.
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_CURRENT_INVALID_HISTORY") !=
          std::string::npos);
    CHECK(harness.find("2147483648u") != std::string::npos);
    CHECK(prepass.find("TFORGE_PREPASS_CURRENT_INVALID_HISTORY") !=
          std::string::npos);
    CHECK(prepass.find(
              "(slot0.z & TFORGE_PREPASS_CURRENT_INVALID_HISTORY) != 0u") !=
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

    // The decoded display image can be larger than the reduced model image.
    // Its sampling must therefore derive its own coordinates from imageSize,
    // not reuse model-space bounds; otherwise a 1280x720 -> 640x360 pass
    // samples only the upper-left half of the display source at the right edge.
    CHECK(postpass.find("const ivec2 displaySize = imageSize(u_sourceDisplay)") !=
          std::string::npos);
    CHECK(postpass.find("displaySize) / vec2(modelSize)") != std::string::npos);
    CHECK(postpass.find("imageLoad(u_sourceDisplay, ivec2(sx, sy))") !=
          std::string::npos);

    // The decoded upload and FSR render grid may differ. The color sampler
    // must receive decoded-pixel jitter, while FSR metadata must receive the
    // equivalent model-pixel displacement at the temporal adapter boundary.
    CHECK(playback.find("jitterToModelX") != std::string::npos);
    CHECK(playback.find("modelJitterX") != std::string::npos);
    CHECK(playback.find("in.jitterX = replaceSyntheticJitter ? 0.0f : modelJitterX") !=
          std::string::npos);

    // The preserved AMD source applies jitter in the FSR prepass resolve, not
    // while decoding YUV. Keep that ordering as an explicit opt-in experiment:
    // PlaybackEngine disables the upload-time shift, the prepass adds the
    // same model-space phase to its input coordinate, and the default path
    // remains byte-for-byte governed by the existing upload jitter flag.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_PREPASS_JITTER_ORDERING") !=
          std::string::npos);
    CHECK(playback.find("prepassJitterOrdering") != std::string::npos);
    // The combined jitter arm must be explicit and must reuse the complete
    // best-findings stack. It is an A/B profile, not a silent production
    // default, so zero-jitter captures remain a valid control.
    CHECK(playback.find("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") !=
          std::string::npos);
    CHECK(playback.find("integratedBestFindingsJitter") != std::string::npos);
    // The named combined-jitter profile must be self-contained at the FSR
    // prepass boundary too. Upstream: the runtime profile selector. Downstream:
    // integrated history/color ordering and temporal-state flags. Without this
    // assertion, a caller using only the named profile can silently receive
    // the ordinary prepass composition even though playback selected jitter.
    CHECK(harness.find("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") !=
          std::string::npos);
    CHECK(uploader.find("inputJitterEnabled") != std::string::npos);
    CHECK(uploaderHeader.find("setInputJitter") != std::string::npos);
    CHECK(prepass.find("TFORGE_PREPASS_JITTER_IN_RESOLVE") !=
          std::string::npos);
    // Source-tap Mu-law ordering was measured as neutral.  It must remain a
    // separately selectable diagnostic rather than becoming an invisible
    // member of the named best-findings stack.  Otherwise a future quality
    // capture cannot attribute a regression or win to its actual stage.
    const size_t sourceTapDefinition = harness.find(
        "const bool sourceTapMuLaw =");
    CHECK(sourceTapDefinition != std::string::npos);
    if (sourceTapDefinition != std::string::npos) {
        const size_t sourceTapEnd = harness.find(";", sourceTapDefinition);
        CHECK(sourceTapEnd != std::string::npos);
        if (sourceTapEnd != std::string::npos) {
            const std::string sourceTapCondition = harness.substr(
                sourceTapDefinition, sourceTapEnd - sourceTapDefinition + 1);
            CHECK(sourceTapCondition.find(
                      "TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") !=
                  std::string::npos);
            // Matched capture proved this exact source-tap representation is
            // required when the named synthetic-jitter arm owns its phase in
            // prepass. Do not pull it into the zero-jitter best-findings
            // profile or unrelated integrated profiles.
            CHECK(sourceTapCondition.find(
                      "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") !=
                  std::string::npos);
            CHECK(sourceTapCondition.find(
                      "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS\")") ==
                  std::string::npos);
            CHECK(sourceTapCondition.find(
                      "TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE") ==
                  std::string::npos);
        }
    }
    const size_t playbackOrdering = playback.find(
        "static const bool prepassJitterOrdering =");
    CHECK(playbackOrdering != std::string::npos);
    if (playbackOrdering != std::string::npos) {
        const size_t playbackOrderingEnd = playback.find(";", playbackOrdering);
        CHECK(playbackOrderingEnd != std::string::npos);
        if (playbackOrderingEnd != std::string::npos) {
            const std::string orderingCondition = playback.substr(
                playbackOrdering, playbackOrderingEnd - playbackOrdering + 1);
            CHECK(orderingCondition.find(
                      "TFORGE_FSR4_EXPERIMENTAL_PREPASS_JITTER_ORDERING") !=
                  std::string::npos);
            CHECK(orderingCondition.find("integratedTemporalProfile") ==
                  std::string::npos);
            // The named synthetic-jitter profile must route its physical
            // phase through the prepass, because postpass already consumes
            // that phase for the learned footprint. Upload-time sampling plus
            // postpass sampling is a two-shift contract violation.
            CHECK(orderingCondition.find("integratedBestFindingsJitter") !=
                  std::string::npos);
        }
    }
    const size_t integratedJitterDefinition = harness.find(
        "const bool integratedJitterProfile =");
    CHECK(integratedJitterDefinition != std::string::npos);
    if (integratedJitterDefinition != std::string::npos) {
        const size_t integratedJitterEnd = harness.find(
            ";", integratedJitterDefinition);
        CHECK(integratedJitterEnd != std::string::npos);
        if (integratedJitterEnd != std::string::npos) {
            const std::string integratedJitterCondition = harness.substr(
                integratedJitterDefinition,
                integratedJitterEnd - integratedJitterDefinition + 1);
            CHECK(integratedJitterCondition.find(
                      "TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") !=
                  std::string::npos);
        }
    }
    const size_t prepassFlag = harness.find("cbData.s0z |= 131072u;");
    CHECK(prepassFlag != std::string::npos);
    if (prepassFlag != std::string::npos) {
        const size_t flagCondition = harness.rfind("if (", prepassFlag);
        CHECK(flagCondition != std::string::npos);
        if (flagCondition != std::string::npos) {
            const std::string condition = harness.substr(
                flagCondition, prepassFlag - flagCondition);
            CHECK(condition.find("integratedJitterProfile") !=
                  std::string::npos);
        }
    }
    // The source-guided per-tap resolve is a separate diagnostic from the
    // production RGB10 path. Its source-coordinate jitter must be converted
    // from the model-pixel phase carried by slot1.zw before sampling source
    // taps; direct addition is wrong whenever source and model sizes differ.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") !=
          std::string::npos);
    CHECK(harness.find("cbData.s0z |= 2048u") != std::string::npos);
    CHECK(prepass.find("TFORGE_PREPASS_SOURCE_TAP_MULAW = 2048u") !=
          std::string::npos);
    CHECK(prepass.find("const vec2 sourceJitter") != std::string::npos);
    CHECK(prepass.find("slot1.zw / max(sourceToModelScale") !=
          std::string::npos);
    CHECK(prepass.find("inputPos += sourceJitter") != std::string::npos);
    CHECK(prepass.find("inputPos += slot1.zw") == std::string::npos);
    // Prepass-owned jitter and source-tap Mu-law are independent controls.
    // A named jitter profile must retain the model-color representation while
    // shifting its resolve; only the explicit source-tap diagnostic may read
    // u_sourceDisplay and apply Mu-law per decoded tap.
    CHECK(prepass.find("useSourceTapMuLaw\n            ? sampleCurrentOfficial") !=
          std::string::npos);
    CHECK(prepass.find("usePrepassJitter || useSourceTapMuLaw\n            ? sampleCurrentOfficial") ==
          std::string::npos);
    // The source-guided resolve must apply the recovered Mu-law transform to
    // each display-space source tap before Gaussian weighting. Filtering the
    // already-transformed RGB10 image is not equivalent to the official path.
    CHECK(prepass.find("u_sourceDisplay") != std::string::npos);
    CHECK(prepass.find("sampleCurrentOfficial") != std::string::npos);
    CHECK(prepass.find("applyMuLawOfficial") != std::string::npos);
    CHECK(prepass.find("sampleCurrentOfficial(inputPos, slot1.xy)") !=
          std::string::npos);
    CHECK(harness.find("w[10].dstBinding = 10") != std::string::npos);
    CHECK(harness.find("in.sourceDisplayView") != std::string::npos);
    CHECK(prepass.find("const vec2 unjitteredInputPos") !=
          std::string::npos);
    CHECK(prepass.find("vec2 motionSamplePos = unjitteredInputPos") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_PREPASS_JITTER_IN_RESOLVE") !=
          std::string::npos);
    CHECK(harness.find("prepassJitterOrdering") != std::string::npos);
    CHECK(harness.find("!prepassJitterOrdering") != std::string::npos);
    // The official-ordering probe normally follows the source contract and
    // leaves the history phase delta off. A separate explicit opt-in must be
    // able to combine the two policies for a matched A/B instead of being
    // silently ignored by the host.
    CHECK(harness.find("!prepassJitterOrdering ||") != std::string::npos);
    CHECK(harness.find("std::getenv(\"TFORGE_FSR4_EXPERIMENTAL_HISTORY_JITTER_DELTA\")") !=
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

    // Native INT8 is the fast convolution graph, not a separate temporal
    // pipeline.  dispatchFrame must still record the prepass and postpass
    // around that graph so motion, jitter, history, and display CAS reach the
    // same output image as the generic graph.  Keep this contract test close
    // to the source so a proof-only early return cannot silently regress the
    // production temporal path again.
    const size_t dispatchStart =
        harness.find("Fsr4DispatchResult Fsr4DispatchHarness::dispatchFrame(");
    const size_t dispatchBody =
        harness.find("auto t0 = std::chrono::steady_clock::now()", dispatchStart);
    CHECK(dispatchStart != std::string::npos);
    CHECK(dispatchBody != std::string::npos);
    if (dispatchStart != std::string::npos &&
        dispatchBody != std::string::npos) {
      const std::string dispatchPreamble =
          harness.substr(dispatchStart, dispatchBody - dispatchStart);
      CHECK(dispatchPreamble.find("if (nativeInt8Active_)") ==
            std::string::npos);
    }
    CHECK(harness.find("recordPrepass(cmd_, in)") != std::string::npos);
    CHECK(harness.find("vkCmdExecuteCommands(cmd_, 1, &nativeInt8Cmd_)") !=
          std::string::npos);

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
    CHECK(harness.find("dispatch trace: temporal flags") != std::string::npos);
    CHECK(harness.find("dispatch trace: {} graph complete") != std::string::npos);
    CHECK(harness.find("dispatch trace: queue submitted") != std::string::npos);

    // The motion-aware learned-strength candidate must consume the same
    // reactive signal already synthesized for the per-frame side buffers.
    // It is opt-in so the current playback contract remains unchanged until
    // matched real-scene captures prove that reducing learned history during
    // active changes improves temporal stability.
    CHECK(harness.find("reactiveAverage") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_ADAPTIVE_LEARNED_STRENGTH") !=
          std::string::npos);
    // A typed Quality Lab learnedStrength must reach the active current
    // composition too. Otherwise a JSON A/B that keeps mode="current" is
    // silently replaced by the legacy scale heuristic and different configs
    // can produce identical pixels while appearing selectable.
    CHECK(harness.find(
              "if (qualityLabConfig_.enabled)\n        learnedStrength = std::clamp(qualityLabConfig_.learnedStrength") !=
          std::string::npos);
    CHECK(harness.find("confidenceGate") != std::string::npos);
    // The aggressive reactive suppression is an experiment, not an implicit
    // property of the promoted best-findings path. Otherwise a reactive
    // average of 1.0 can force the final learned blend to zero and make the
    // temporal output silently collapse to the spatial base.
    const auto adaptiveGate = harness.find(
        "if (bestFindingsTemporal ||\n          std::getenv(\"TFORGE_FSR4_ADAPTIVE_LEARNED_STRENGTH\"))");
    CHECK(adaptiveGate == std::string::npos);
    CHECK(harness.find(
              "if (std::getenv(\"TFORGE_FSR4_ADAPTIVE_LEARNED_STRENGTH\"))") !=
          std::string::npos);

    // The promoted best-findings path is the normal temporal path. A single
    // explicit disable switch keeps controlled A/B capture possible without
    // making the review harness depend on process-global setup variables.
    // Upstream: decoded frames and existing campaign-tested helpers.
    // Downstream: motion upload, temporal flags, postpass composition, and
    // the display CAS stage. FSR1/EASU remains opt-in because its matched
    // evidence did not establish a safe universal gain.
    CHECK(playback.find("TFORGE_FSR4_DISABLE_BEST_FINDINGS") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_DISABLE_BEST_FINDINGS") !=
          std::string::npos);
    CHECK(harness.find("bestFindingsTemporal") != std::string::npos);
    CHECK(playback.find("bestFindingsTemporal") != std::string::npos);
    // Best-findings owns persistent history/recurrent state even when the
    // caller did not repeat the enabling environment variables. The async
    // two-slot path must therefore be excluded explicitly, or a failed
    // in-flight frame can advance jitter without publishing matching history.
    const size_t asyncSlots = playback.find("const bool asyncSlots =");
    CHECK(asyncSlots != std::string::npos);
    if (asyncSlots != std::string::npos) {
        const size_t asyncSlotsEnd = playback.find(";", asyncSlots);
        CHECK(asyncSlotsEnd != std::string::npos);
        if (asyncSlotsEnd != std::string::npos) {
            const std::string asyncBlock = playback.substr(
                asyncSlots, asyncSlotsEnd - asyncSlots);
            CHECK(asyncBlock.find("!bestFindingsTemporal") != std::string::npos);
        }
    }
    CHECK(playback.find("MOTION_REFINE_RADIUS") != std::string::npos);
    CHECK(harness.find("POSTPASS_CURRENT_WEIGHT") != std::string::npos);

    // Display-color history has a separate opt-in confidence threshold. The
    // learned-strength gate alone cannot stop a bad codec vector from
    // pulling stale color into the prepass history image.
    CHECK(harness.find("TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD") !=
          std::string::npos);
    // The learned-output confidence curve must be runtime-configurable too.
    // Otherwise changing the documented history threshold only changes the
    // prepass gate while the postpass continues using hidden constants.
    CHECK(harness.find("TFORGE_FSR4_LEARNED_CONFIDENCE_FLOOR") !=
          std::string::npos);
    CHECK(harness.find("learnedConfidenceFloor") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_DISABLE_HISTORY_JITTER_DELTA") !=
          std::string::npos);
    CHECK(prepass.find("historyConfidenceThreshold") != std::string::npos);
    CHECK(prepass.find("HISTORY_CONFIDENCE_GATE") != std::string::npos);

    // The rectification multiplier is a diagnostic-only control. It must be
    // carried through the existing prepass constants, applied to the gated
    // current/history weight, and default to exactly 1.0. Upstream: the
    // benchmark environment; downstream: only temporal history composition.
    CHECK(harness.find("TFORGE_FSR4_HISTORY_RECTIFICATION_SCALE") !=
          std::string::npos);
    CHECK(harness.find("cbData.s3y") != std::string::npos);
    CHECK(prepass.find("historyRectificationScale") != std::string::npos);
    CHECK(prepass.find("temporalSampleWeight *= historyRectificationScale") !=
          std::string::npos);

    // A native passthrough frame and a neural frame may be adjacent when the
    // target ratio changes. The published-image accessor must not keep serving
    // the previous raw image after neural publication has succeeded.
    const auto neuralPublish = playback.find(
        "fsr4PublishedUploader_.store(presentationUploader,");
    CHECK(neuralPublish != std::string::npos);
    CHECK(playback.find("fsr4NativePassthrough_.store(false",
                        neuralPublish) != std::string::npos);

    // Temporal ping-pong state belongs to a frame only after its presentation
    // scaler succeeds. Advancing history immediately after dispatch would
    // let a failed presentation contaminate the next frame's reprojection.
    const auto firstPresentation = playback.find(
        "if (!firstUploader->dispatchPresentationScaler(displayW");
    const auto firstAdvance = playback.find(
        "firstUploader->advanceHistory();", firstPresentation);
    CHECK(firstAdvance != std::string::npos);
    CHECK(firstPresentation != std::string::npos);
    CHECK(firstAdvance > firstPresentation);
    const auto chainedAdvance = playback.find(
        "uploaderAt(pass)->advanceHistory();");
    const auto chainedPresentation = playback.find(
        "dispatchPresentationScaler(displayW");
    CHECK(chainedAdvance != std::string::npos);
    CHECK(chainedPresentation != std::string::npos);
    CHECK(chainedAdvance > chainedPresentation);

    if (g_failures == 0) {
        std::printf("fsr4_temporal_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_temporal_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
