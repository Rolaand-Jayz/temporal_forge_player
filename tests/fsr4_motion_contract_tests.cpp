// fsr4_motion_contract_tests.cpp — M3 tests for causal codec motion.
//
// These tests lock the current motion contract at its boundaries: FFmpeg
// source direction, CPU causal filtering/scaling, GPU sparse expansion, and
// confidence/reset handoff into temporal dispatch.
#include <cstdio>
#include <algorithm>
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
    const std::string decoderHeader = readSource("src/media/VideoDecoder.hpp");
    const std::string playback = readSource("src/core/PlaybackEngine.cpp");
    const std::string playbackHeader = readSource("src/core/PlaybackEngine.hpp");
    const std::string mainSource = readSource("src/main.cpp");
    const std::string runner =
        readSource("benchmarks/video_corpus/run_temporal_quality.sh");
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
    const std::string fp16Pointwise =
        readSource("shaders/fsr4/conv_pw_fp16_direct.comp");
    const std::string fp16Spatial =
        readSource("shaders/fsr4/conv_spatial_fp16_direct.comp");
    const std::string harness = readSource("src/render/Fsr4DispatchHarness.cpp");

    CHECK(decoder.find("static_cast<int8_t>(std::clamp(") !=
          std::string::npos);
    // A B-picture's negative source index identifies a past reference list
    // entry, not necessarily the immediately previous displayed frame. The
    // integrated causal profile must be able to identify and reject that
    // ambiguous case before it reaches history reprojection.
    CHECK(decoder.find("AV_PICTURE_TYPE_B") != std::string::npos);
    CHECK(decoderHeader.find("bFrame") != std::string::npos);
    CHECK(playback.find("rejectBFrameMotion") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_MOTION_ALLOW_B_FRAMES") !=
          std::string::npos);
    CHECK(playback.find("pastReferenceMotion(\n          causalSeeds,") !=
          std::string::npos);
    // Timescale normalization: the causal seed copy must come straight from
    // the decoded frame, and a P-picture group distance must reach the
    // rescale so IBBP vectors estimate the same per-display-frame
    // correspondence as B-picture back vectors.
    CHECK(playback.find("std::vector<MvEntry> causalSeeds = df.motionVectors;") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_MOTION_TIMESCALE_NORMALIZE") !=
          std::string::npos);
    CHECK(decoderHeader.find("mvReferenceDistance") != std::string::npos);
    CHECK(decoder.find("mvReferenceDistance") != std::string::npos);
    // A requested codec/refined estimator needs FFmpeg side data, which the
    // VAAPI frame handoff does not preserve. The decoder must therefore make
    // that selection truthful instead of silently running with zero seeds.
    CHECK(decoder.find("motionMetadataRequested") != std::string::npos);
    CHECK(decoder.find("TFORGE_FSR4_MOTION_ESTIMATOR") !=
          std::string::npos);
    // The capture runner also selects the refined estimator through the
    // ablation label. Decoder setup must honor that same label when deciding
    // whether hardware decode would discard FFmpeg motion side data.
    CHECK(decoder.find("TFORGE_FSR4_MOTION_ABLATION") !=
          std::string::npos);
    CHECK(decoder.find("std::strcmp(motionMode, \"refined\")") !=
          std::string::npos);
    // A typed Quality Lab motion selector is resolved after the engine loads
    // its config but before VideoDecoder::open. The decoder must receive that
    // request explicitly; otherwise VAAPI can discard the side-data before
    // the estimator ever sees it.
    CHECK(decoderHeader.find("setMotionMetadataRequested") !=
          std::string::npos);
    CHECK(playback.find("setMotionMetadataRequested(true)") !=
          std::string::npos);
    // An explicit off arm is a real control, not merely a zeroed texture
    // after decoder setup. It must prevent a typed Quality Lab motion block
    // from forcing software decode and paying for side-data that the control
    // intentionally does not consume.
    CHECK(playback.find("environmentMotionModeIsOff") != std::string::npos);
    CHECK(playback.find("!environmentMotionModeIsOff") != std::string::npos);
    // The runner can request the standalone estimator through the ablation
    // label alone. The analysis-luma builder must recognize that same request
    // or it silently falls back to the legacy 96-pixel grid, making the
    // estimator's source-pixel correction bound reject useful refinements.
    CHECK(playback.find("motionEstimatorRequested") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_MOTION_ABLATION") !=
          std::string::npos);
    // A typed Quality Lab motion block must select the same dense analysis
    // grid even when no environment selector is present.
    CHECK(playback.find("const LumaBuffer analysisLuma = makeAnalysisLuma(") !=
          std::string::npos);
    CHECK(playback.find(
              "df, effectiveMotionConfig.mode != MotionEstimatorMode::Off") !=
          std::string::npos);
    // An explicit Quality Lab motion block must win over ambient environment
    // state; otherwise two captures with the same JSON can run different
    // estimators and produce incomparable temporal evidence.
  CHECK(playback.find("qualityLabConfig_.motionConfigured") !=
        std::string::npos);
  // An explicit benchmark motion selector must override only the configured
  // estimator mode, while retaining the JSON tuning values. Otherwise a
  // capture labelled refined can silently execute the configured codec arm.
  CHECK(playback.find("TFORGE_FSR4_MOTION_ESTIMATOR") != std::string::npos);
  CHECK(playback.find("effectiveMotionConfig.mode = environmentMotionConfig.mode") !=
        std::string::npos);
  // Explicit dense-grid capture controls must override only the typed motion
  // policy flag; otherwise the runner can claim a dense A/B arm while the
  // JSON profile silently keeps the seed-only estimator.
  CHECK(playback.find("TFORGE_FSR4_MOTION_DENSE_GRID") !=
        std::string::npos);
  CHECK(playback.find("effectiveMotionConfig.denseGridFallback") !=
        std::string::npos);
    CHECK(playback.find("? qualityLabConfig_.motion") != std::string::npos);
    // Persisted motion policy must reach the engine when no typed campaign
    // motion block overrides it; otherwise the user-facing AutoCheap default
    // exists only in settings.json and cannot affect live playback.
    CHECK(playbackHeader.find("setMotionMode") != std::string::npos);
    CHECK(mainSource.find("engine.setMotionMode(settings.motionMode)") !=
          std::string::npos);
    CHECK(playback.find("MotionMode::AutoCheap") != std::string::npos);
    CHECK(playback.find("MotionMode::Block") != std::string::npos);
    CHECK(playback.find("MotionMode::Zero") != std::string::npos);
    CHECK(playback.find("mv.source <= 0") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvX)") != std::string::npos);
    CHECK(playback.find("std::isfinite(mv.mvY)") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS") !=
          std::string::npos);
    CHECK(playback.find("motionLimitMultiplier") != std::string::npos);
    // Coverage positions and motion magnitudes are both converted into model
    // pixels before upload. The prepass then applies model-to-output scaling;
    // this keeps reduced-model paths from over-scaling source-pixel vectors.
    CHECK(playback.find("std::vector<MvEntry> scaleMotionCoverageToModel") !=
          std::string::npos);
    CHECK(playback.find("mv.mvX *= sx") != std::string::npos);
    CHECK(playback.find("mv.mvY *= sy") != std::string::npos);
    // Motion analysis must read the decoded luma plane according to its
    // component depth. Treating a 10/12-bit packed plane as one-byte samples
    // corrupts refinement, validation, and scene-cut evidence before FSR.
    CHECK(playback.find("const bool highBitDepth = frame.bitDepth > 8") !=
          std::string::npos);
    CHECK(playback.find("const size_t bytesPerSample = highBitDepth ? 2u : 1u") !=
          std::string::npos);
    CHECK(playback.find("frame.plane[0][byteOffset + 1u]") !=
          std::string::npos);
    CHECK(playback.find("1u << analysisBitDepth") != std::string::npos);

    // The uploader owns model-sized resources. A reduced-model path must be
    // compared against the selected model dimensions, not decoded dimensions,
    // or every frame will silently rebuild back to the source size.
    CHECK(playback.find("configuredInput->modelW() != fsrModelW") !=
          std::string::npos);
    CHECK(playback.find("configuredInput->modelH() != fsrModelH") !=
          std::string::npos);
    CHECK(playback.find("scaleMotionCoverageToModel(\n            pastMotion") !=
          std::string::npos);
    // Uploaded motion is in model pixels. Reduced models therefore need the
    // model-to-output ratio, not the decoded-source-to-output ratio.
    CHECK(prepass.find("imageLoad(u_motion, motionCoord).xy *\n                          modelToOutputScale") !=
          std::string::npos);
    CHECK(prepass.find("imageLoad(u_motion, motionCoord).xy *\n                          slot1.xy") ==
          std::string::npos);
    // The motion image is model-sized, while motionSamplePos is in decoded
    // source pixels. Sampling it without the source-to-model coordinate map
    // collapses all source pixels beyond the model edge onto one border vector
    // whenever an intentionally smaller neural model is used.
    CHECK(prepass.find("motionSamplePos * sourceToModelScale") !=
          std::string::npos);
    // Non-identity neural models need two distinct ratios: slot1.xy remains
    // source-to-output for history displacement, while model-sized color and
    // motion images require explicit source-to-model and model-to-output maps.
    CHECK(prepass.find("sourceToModelScale") != std::string::npos);
    CHECK(prepass.find("modelToOutputScale") != std::string::npos);
    // Both graph families read the first FP16 feature tensor from
    // finalTensorBuffer. A split destination would silently disconnect
    // motion/history features from generic pass 0.
    CHECK(harness.find("const VkBuffer prepassOutputBuffer") !=
          std::string::npos);
  CHECK(harness.find("const VkBuffer prepassOutputBuffer = res_.finalTensorBuffer") !=
        std::string::npos);
  CHECK(harness.find("const uint32_t prepassWidth = res_.outputWidth") !=
        std::string::npos);
  CHECK(harness.find("nativeInt8Active_\n                                    ? std::max(1u, res_.outputWidth / 2u)") ==
        std::string::npos);
  CHECK(harness.find("(prepassWidth + 31u) / 32u") != std::string::npos);
  CHECK(harness.find("if (nativeInt8Active_)") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION") !=
          std::string::npos);
    CHECK(sideSynth.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_RADIUS") !=
          std::string::npos);
    CHECK(sideSynth.find("fallbackMotionSearchRadius") != std::string::npos);
    CHECK(sideSynth.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_COARSE_TO_FINE") !=
          std::string::npos);
    CHECK(sideSynth.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION_BLOCK_SIZE") !=
          std::string::npos);
    CHECK(sideSynth.find("fuseBidirectionalMotion") != std::string::npos);
    CHECK(sideSynth.find("gateMotionWithFutureEvidence") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_BIDIRECTIONAL_MOTION") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY") !=
          std::string::npos);
    // The display-interpolation probe must be an explicit runner-forwarded
    // opt-in. It is the only path that turns future decoded pixels into the
    // actual FSR input/display candidate; the default remains causal.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_DISPLAY_INTERPOLATED") !=
          std::string::npos);
    // Interpolated jitter replacement must keep the decoded output cadence
    // while replacing synthetic jitter with a real inter-frame sample.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_INTERPOLATED_JITTER") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_EXPERIMENTAL_INTERPOLATED_JITTER") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_FUTURE_ALIGNED_JITTER") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_EXPERIMENTAL_FUTURE_ALIGNED_JITTER") !=
          std::string::npos);
    // Enabling the future-aligned mode must also enable packet lookahead;
    // otherwise the named experiment silently runs the ordinary causal path.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_FUTURE_ALIGNED_JITTER") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_FUTURE_ALIGN_PHOTOMETRIC_THRESHOLD") !=
          std::string::npos);
    // Both interpolation variants replace the synthetic phase offsets. The
    // future-aligned variant must not silently retain the old jitter path.
    CHECK(playback.find("interpolatedJitterEnv || futureAlignedJitterEnv") !=
          std::string::npos);
    CHECK(playback.find("futureAlignPhotometricThreshold") !=
          std::string::npos);
    // Future-aligned history A/B captures must expose the detector/reset and
    // motion handoff in profile logs; identical pixels are not evidence that
    // history is harmless unless the runtime proves whether history ran.
    CHECK(playback.find("future-aligned handoff frame=") !=
          std::string::npos);
    // Variable jitter must be configured from the current FSR model/output
    // pair before update() chooses its phase. Otherwise the first frame after
    // a resolution or scale change is compared with a stale phase cadence.
    const size_t jitterPairInstall =
        playback.find("sideBufferSynth_.setFsrJitterPair(\n"
                      "            jitterPair.modelW");
    const size_t sideInputUpdate = playback.find(
        "const SideBufferInputs sideInputs = sideBufferSynth_.update(");
    CHECK(jitterPairInstall != std::string::npos);
    CHECK(sideInputUpdate != std::string::npos);
    CHECK(jitterPairInstall < sideInputUpdate);
    CHECK(playback.find("computeFsrJitterPair") != std::string::npos);
    // A forced 1920x1080 viewport must not bypass FSR when the selected
    // quality multiplier still reconstructs on a smaller model grid. The
    // 1080p source tier therefore remains a real motion-input experiment.
    const size_t passthrough = playback.find("pair.nativePassthrough =");
    CHECK(passthrough != std::string::npos);
    if (passthrough != std::string::npos) {
      const size_t end = playback.find(';', passthrough);
      const std::string condition = playback.substr(passthrough, end - passthrough);
      CHECK(condition.find("modelW == decodedW") != std::string::npos);
      CHECK(condition.find("modelH == decodedH") != std::string::npos);
    }
    // The jitter-sign probe must alter the uploaded color sample and the FSR
    // metadata as one contract. A sign change in only one side would create a
    // deliberately mismatched temporal input and make the A/B result useless.
    CHECK(playback.find("TFORGE_FSR4_JITTER_SIGN") != std::string::npos);
    CHECK(playback.find("jitterSign") != std::string::npos);
    CHECK(playback.find("sideInputs.jitterX * jitterSign") !=
          std::string::npos);
    CHECK(playback.find("modelJitterX = sideInputs.jitterX * jitterToModelX * jitterSign") !=
          std::string::npos);
    // A separate sample-sign probe is needed to distinguish a complete
    // convention flip from a mismatch between the physical color sample and
    // the jitter value reported to FSR. It must affect only the upload-side
    // sample; the normal/default contract remains unchanged.
    CHECK(playback.find("TFORGE_FSR4_JITTER_SAMPLE_SIGN") !=
          std::string::npos);
    CHECK(playback.find("jitterSampleSign") != std::string::npos);
    CHECK(playback.find("sideInputs.jitterX * jitterSign * jitterSampleSign") !=
          std::string::npos);
    // Synthetic jitter is useful for controlled video experiments, but it is
    // not guaranteed to add information to an already-rasterized frame. The
    // production path must therefore default to zero jitter while retaining
    // an explicit synthetic opt-in.
    CHECK(playback.find("std::strcmp(jitterModeEnv, \"synthetic\") == 0") !=
          std::string::npos);
    CHECK(playback.find("sideBufferSynth_.setJitterMode(JitterMode::Off);") !=
          std::string::npos);
    CHECK(playback.find("// The default is zero jitter") != std::string::npos);

    // Motion magnitudes enter the dense image in model-pixel units. The
    // prepass then converts model pixels to output pixels with output/model;
    // retaining source-pixel magnitudes would over-scale reduced-model paths.
    CHECK(playback.find("mv.mvX *= sx") != std::string::npos);
    CHECK(playback.find("mv.mvY *= sy") != std::string::npos);

    // A resolution change changes the coordinate units of the stored prior
    // jitter. The next frame must not reuse that value or compare history
    // against a phase from the old render-size pair.
    CHECK(playback.find("renderSizeChanged") != std::string::npos);
    const size_t renderSizeChanged = playback.find("renderSizeChanged");
    CHECK(playback.find("hasPreviousJitter = false", renderSizeChanged) !=
          std::string::npos);

    // SideBufferSynth::update() advances a candidate jitter phase before the
    // FSR path knows whether this decoded frame can be submitted. Playback
    // must therefore install the default rollback side of its
    // rollback/commit guard before any pre-dispatch exit, including the
    // frame-ready wait and the FSR-disabled/abort gate. The commit side may
    // only run after a submitted dispatch has completed successfully.
    const size_t firstSubmittedFsrDispatchAsync = playback.find(
        "firstHarness->dispatchFrameAsync(in)", sideInputUpdate);
    const size_t firstSubmittedFsrDispatchSync = playback.find(
        "firstHarness->dispatchFrame(in)", sideInputUpdate);
    size_t firstSubmittedFsrDispatch = std::string::npos;
    if (firstSubmittedFsrDispatchAsync != std::string::npos &&
        firstSubmittedFsrDispatchSync != std::string::npos) {
        firstSubmittedFsrDispatch = std::min(firstSubmittedFsrDispatchAsync,
                                             firstSubmittedFsrDispatchSync);
    } else if (firstSubmittedFsrDispatchAsync != std::string::npos) {
        firstSubmittedFsrDispatch = firstSubmittedFsrDispatchAsync;
    } else {
        firstSubmittedFsrDispatch = firstSubmittedFsrDispatchSync;
    }
    const size_t fsrGate = playback.find(
        "if (fsr4Enabled_.load(std::memory_order_acquire)", sideInputUpdate);
    const size_t waitExit = playback.find(
        "if (!running_.load() || seekPending_.load())", sideInputUpdate);
    const size_t waitContinue = playback.find("continue;", waitExit);
    const size_t firstJitterRollback = playback.find(
        "sideBufferSynth_.rollbackJitter(", sideInputUpdate);
    const size_t jitterCommit = playback.find(
        "sideBufferSynth_.commitJitter()", sideInputUpdate);

    CHECK(firstSubmittedFsrDispatch != std::string::npos);
    CHECK(fsrGate != std::string::npos);
    CHECK(waitExit != std::string::npos);
    CHECK(waitContinue != std::string::npos);
    CHECK(firstJitterRollback != std::string::npos);
    CHECK(jitterCommit != std::string::npos);
    // These are intentionally test-first: the current manual rollback in the
    // dispatch-result branch is too late to guard the early exits above.
    CHECK(firstJitterRollback < fsrGate);
    CHECK(firstJitterRollback < waitContinue);
    CHECK(firstJitterRollback < firstSubmittedFsrDispatch);
    // Preserve the other half of the contract: a successful commit belongs
    // after the first actual FSR submission, not at update() time.
    CHECK(firstSubmittedFsrDispatch < jitterCommit);

    // PTS analysis state is part of the same frame transaction as jitter and
    // motion history. A failed upload/dispatch must not make an unpublished
    // frame the timestamp origin for the next successful frame.
    CHECK(playback.find("analysisPtsBeforeFrame") != std::string::npos);
    CHECK(playback.find("lastAnalysisPtsUs_ = analysisPtsBeforeFrame") !=
          std::string::npos);
    const size_t analysisRollback = playback.find(
        "lastAnalysisPtsUs_ = analysisPtsBeforeFrame");
    CHECK(analysisRollback > firstJitterRollback);
    CHECK(analysisRollback < jitterCommit);

    // Previous-jitter metadata is consumed by the next frame's history
    // reprojection. A presentation failure must roll it back with the phase
    // and PTS, or the next frame pairs an unpublished jitter phase with
    // published history.
    CHECK(playback.find("previousJitterXBeforeFrame") != std::string::npos);
    CHECK(playback.find("previousJitterYBeforeFrame") != std::string::npos);
    CHECK(playback.find("hasPreviousJitterBeforeFrame") != std::string::npos);
    const size_t previousJitterRollback = playback.find(
        "previousJitterX = previousJitterXBeforeFrame");
    CHECK(previousJitterRollback != std::string::npos);
    CHECK(previousJitterRollback > firstJitterRollback);
    CHECK(previousJitterRollback < jitterCommit);

    // A forced reset is consumed when the candidate frame begins, but it is
    // still owed to the next frame if upload, dispatch, or presentation
    // fails. Otherwise a newly recreated temporal resource can be sampled
    // without the reset bit that makes the first successful frame safe.
    CHECK(playback.find(
              "const bool requestedTemporalReset =\n          fsrTemporalResetRequested_.exchange(false") !=
          std::string::npos);
    CHECK(playback.find(
              "fsrTemporalResetRequested_.store(true, std::memory_order_release)") !=
          std::string::npos);
    const size_t resetRequestRestore = playback.find(
        "fsrTemporalResetRequested_.store(true, std::memory_order_release)",
        firstJitterRollback);
    CHECK(resetRequestRestore > firstJitterRollback);
    CHECK(resetRequestRestore < jitterCommit);

    // Dense replay is capture-relative and decoder seeks restart decoder
    // frame numbering. The replay cache must therefore be invalidated or
    // rebased at a seek generation boundary instead of retaining the old
    // frameBase across a seek.
    CHECK(playback.find("denseMotionReplaySeekGeneration") !=
          std::string::npos);
    CHECK(playback.find("cache.seekGeneration") != std::string::npos);
    CHECK(playback.find("cache.frameBaseSet = false") != std::string::npos);

    // The future-aligned opt-in must not silently enter a reset-only state on
    // clips without codec vectors; it may use the bounded existing matcher as
    // an explicit motion source for that opt-in candidate.
    CHECK(playback.find("futureAlignedFallbackMotion") !=
          std::string::npos);
    CHECK(playback.find("makeFutureAlignedFrame(df, pendingDecodedFrame,") !=
          std::string::npos);
    // Dense replay tiles must carry their validated confidence into the GPU
    // validity map; silently treating every tile as confidence 1.0 would make
    // the confidence-aware motion experiment untestable.
    CHECK(playback.find("item.value(QStringLiteral(\"confidence\"))") !=
          std::string::npos);
    // The normal display path uses one post-reconstruction CAS stage at the
    // game-tuned strength; an environment value may still override it for a
    // measured experiment.
    CHECK(harness.find("kDisplayCasStrength = 0.20f") != std::string::npos);
    // The historical RCAS-like sharpen must be neutral in the normal path;
    // otherwise the display receives two sharpen stages instead of the one
    // game-matched CAS stage above.
    CHECK(harness.find("float legacyRcasStrength = 0.0f") !=
          std::string::npos);
    // The display composition owns exactly one CAS invocation. This is a
    // source-level guard against accidentally stacking another display
    // sharpen stage while changing the temporal composition branches.
    const std::string casInvocation = "finalColor = applyCas(";
    const size_t firstCasInvocation = postpass.find(casInvocation);
    CHECK(firstCasInvocation != std::string::npos);
    CHECK(postpass.find(casInvocation, firstCasInvocation + 1) ==
          std::string::npos);


    // The FP16 direct fallback normally preserves the existing carrier. The
    // diagnostic boundary must be explicitly selectable so the generic graph
    // can be compared with and without its recovered FP8 CopySat boundary.
    CHECK(harness.find("TFORGE_FSR4_FP16_FP8_BOUNDARY") !=
          std::string::npos);
    CHECK(fp16Pointwise.find("slot2.y != 2") != std::string::npos);
    CHECK(fp16Spatial.find("slot2.y != 2") != std::string::npos);
    // An interpolated display frame cannot inherit motion/history metadata
    // computed for the original current frame. The probe must derive a
    // midpoint field from both adjacent directions and fail closed when that
    // field is unavailable, rather than pairing midpoint pixels with
    // unrelated temporal coordinates.
    CHECK(playback.find("fuseBidirectionalMotion(") != std::string::npos);
    CHECK(playback.find("interpolatedMotion.empty()") != std::string::npos);
    CHECK(playback.find("interpolatedFrame.motionVectors.clear()") !=
          std::string::npos);
    CHECK(playback.find("interpolatedFrameTemporalReset") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_EXPERIMENTAL_DISPLAY_INTERPOLATED") !=
          std::string::npos);
    CHECK(playback.find("Future-frame probes need one decoded lookahead frame") !=
          std::string::npos);
    CHECK(playback.find("videoPackets_.push_front") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION") !=
          std::string::npos);
    // The promoted best-findings path must not silently pay for the rejected
    // CPU refinement probe. Refinement remains an explicit diagnostic so the
    // raw codec-motion result can be reproduced without source changes.
    CHECK(playback.find(
              "const bool legacyMotionRefinement =\n          std::getenv(\"TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION\") != nullptr;") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_MOTION_ABLATION") !=
          std::string::npos);
    // A zero-motion A/B must not change the confidence used by the reset and
    // jitter policy; otherwise the control can take a different scene-cut
    // path and receive a different variable-jitter sequence.
    CHECK(playback.find("preAblationMotionConfidence") != std::string::npos);
    CHECK(playback.find("zeroMotionAblation") != std::string::npos);
    CHECK(playback.find("sideMotionConfidence") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION_HYBRID") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_VALIDATE_MOTION") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_CONFIDENCE_MAP") !=
          std::string::npos);
    // The integrated video profile must combine the validated causal motion
    // path with source-space color ordering without requiring callers to
    // remember a fragile collection of independent environment switches.
    CHECK(playback.find("TFORGE_FSR4_INTEGRATED_TEMPORAL") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS") !=
          std::string::npos);
    // Reprojection diagnostics must not be coupled to the presentation PPM
    // stream's health: a completed dense-motion readback still needs its
    // matching FP16 history-warp artifact when that diagnostic is enabled.
    CHECK(playback.find("if (dumpReprojectedColorEnv) {") !=
          std::string::npos);
    // Overlapping sparse blocks must have an explicit, opt-in confidence
    // ordering before the GPU/CPU last-writer coverage rule. This keeps the
    // existing baseline unchanged while making the diagnostic arm independent
    // of codec vector upload order.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_CONFIDENCE_ORDERED_MOTION") !=
          std::string::npos);
    // Best-findings already enables per-vector confidence scoring. It must
    // also make the GPU's last-writer rule deterministic by ordering the
    // scored blocks, while raw/baseline arms retain their original behavior.
    const size_t confidenceOrderIf = playback.find(
        "if (!pastMotion.empty() &&\n          (bestFindingsTemporal ||");
    CHECK(confidenceOrderIf != std::string::npos);
    if (confidenceOrderIf != std::string::npos) {
        const size_t confidenceOrderEnd = playback.find(
            "orderMotionByConfidence(pastMotion);", confidenceOrderIf);
        CHECK(confidenceOrderEnd != std::string::npos);
        if (confidenceOrderEnd != std::string::npos) {
            const std::string confidenceOrderBlock = playback.substr(
                confidenceOrderIf, confidenceOrderEnd - confidenceOrderIf);
            CHECK(confidenceOrderBlock.find(
                      "TFORGE_FSR4_EXPERIMENTAL_CONFIDENCE_ORDERED_MOTION") !=
                  std::string::npos);
        }
    }
    CHECK(playback.find("stable_sort") != std::string::npos);
    // Motion analysis resolution is an explicit data-quality control. The
    // default remains the established cheap path; the opt-in must be bounded
    // so denser correspondence cannot silently become an unbounded CPU cost.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_ANALYSIS_WIDTH") !=
          std::string::npos);
    CHECK(playback.find("std::clamp(parsed, 32u, 384u)") !=
          std::string::npos);
    // The Phase 3 confidence candidate must be a distinct opt-in path that
    // evaluates the current pixel against its reprojected history. A block
    // confidence value copied across the target grid is not per-pixel
    // history validation and must not be mistaken for it.
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_PHOTOMETRIC_HISTORY_GATE") !=
          std::string::npos);
    // The promoted profile normally enables this per-pixel gate, but a
    // matched temporal experiment must be able to turn off this one policy
    // without also disabling codec motion, recurrent state, or the other
    // retained best-findings controls.
    CHECK(harness.find("TFORGE_FSR4_DISABLE_PHOTOMETRIC_HISTORY_GATE") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_DISABLE_PHOTOMETRIC_HISTORY_GATE") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS") !=
          std::string::npos);
    // The frame-level gate must align with the adaptive confidence floor. A
    // default above 0.55 rejects the measured ~0.64-0.77 codec confidence
    // range too aggressively before per-pixel confidence can do its work.
    CHECK(harness.find("thresholdEnv = \"0.55\"") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_CONFIDENCE_MAP") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_RECURRENT_CURRENT_COORD") !=
          std::string::npos);
    CHECK(sideSynth.find("validateCodecMotion") != std::string::npos);
    CHECK(sideSynth.find("scoreCodecMotion") != std::string::npos);
    CHECK(playback.find("loadDenseMotionReplay") != std::string::npos);
    CHECK(playback.find("frameIndex - cache.frameBase") != std::string::npos);
    CHECK(playback.find("frameBaseSet") != std::string::npos);
    CHECK(playback.find("df.frameIndex - dumpSequenceWarmup") !=
          std::string::npos);
    CHECK(playback.find("replayFrameReady") != std::string::npos);
    CHECK(playback.find("refineCodecMotion") != std::string::npos);
    CHECK(sideSynthHeader.find("refineCodecMotion") != std::string::npos);
    // Every refinement entry point must enforce the same causal reference
    // contract as the replay adapter: only source=-1 means the immediately
    // previous displayed frame. Zero is ambiguous, not a safe past frame.
    const size_t refineStart = sideSynth.find(
        "std::vector<MvEntry> SideBufferSynth::refineCodecMotion");
    const size_t refineEnd = sideSynth.find(
        "std::vector<MvEntry> SideBufferSynth::validateCodecMotion", refineStart);
    CHECK(refineStart != std::string::npos);
    CHECK(refineEnd != std::string::npos);
    if (refineStart != std::string::npos && refineEnd != std::string::npos) {
        CHECK(sideSynth.substr(refineStart, refineEnd - refineStart)
                  .find("source != -1") != std::string::npos);
    }
    // Validation and confidence scoring are also causal history boundaries,
    // not generic codec-vector utilities. They must reject source=0
    // (ambiguous/current) and older/future references just like refinement,
    // the decoder handoff, and dense replay. Downstream: only an
    // immediately-previous-frame seed may influence temporal history.
    const size_t validateStart = sideSynth.find(
        "std::vector<MvEntry> SideBufferSynth::validateCodecMotion");
    const size_t scoreStart = sideSynth.find(
        "std::vector<MvEntry> SideBufferSynth::scoreCodecMotion");
    CHECK(validateStart != std::string::npos);
    CHECK(scoreStart != std::string::npos);
    if (validateStart != std::string::npos && scoreStart != std::string::npos) {
        CHECK(sideSynth.substr(validateStart, scoreStart - validateStart)
                  .find("source != -1") != std::string::npos);
    }
    if (scoreStart != std::string::npos) {
        CHECK(sideSynth.substr(scoreStart).find("source != -1") !=
              std::string::npos);
    }
    CHECK(sideSynth.find("localSad") != std::string::npos);
    CHECK(sideSynth.find("refinementRadius") != std::string::npos);
    // Both refinement implementations must bound corrections in source-pixel
    // units; quarter-resolution integer searches otherwise create false
    // four-pixel jumps from valid fractional codec seeds.
    CHECK(sideSynthHeader.find("maxCorrectionPixels") != std::string::npos);
    CHECK(sideSynth.find("std::hypot(correctionX, correctionY)") !=
          std::string::npos);
    CHECK(playback.find("maxCorrectionPixels") != std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE") !=
          std::string::npos);
    CHECK(playback.find("frameIndex == 0 ? std::vector<MvEntry>{}") !=
          std::string::npos);
    // An explicit codec selector must mean raw codec motion. The legacy
    // best-findings refinement is only allowed when no standalone estimator
    // mode was selected; otherwise a capture labeled `codec` can silently
    // contain a second refinement stage.
    CHECK(playback.find("motionEstimatorConfig.mode == MotionEstimatorMode::Off") !=
          std::string::npos);

    // Dense replay is a causal history input, so only the immediately
    // previous-reference marker is admissible. Motion components must also
    // remain within the decoded source dimensions before they are narrowed
    // into the runtime vector/FP16 motion path.
    const size_t replayStart = playback.find("bool loadDenseMotionReplay(");
    const size_t replayEnd = playback.find("\nfloat lookaheadConfidence(", replayStart);
    CHECK(replayStart != std::string::npos);
    CHECK(replayEnd != std::string::npos);
    if (replayStart != std::string::npos && replayEnd != std::string::npos) {
        const std::string replay =
            playback.substr(replayStart, replayEnd - replayStart);
        const size_t replaySourceGuard = replay.find("source != -1");
        const size_t motionXBound =
            replay.find("std::abs(mvX) > static_cast<double>(sourceW)");
        const size_t motionYBound =
            replay.find("std::abs(mvY) > static_cast<double>(sourceH)");
        const size_t motionXConversion =
            replay.find("motion.mvX = static_cast<float>(mvX)");
        const size_t motionYConversion =
            replay.find("motion.mvY = static_cast<float>(mvY)");
        const size_t runtimeSourceAssignment = replay.find("motion.source =");
        const size_t motionUpload = replay.find("vectors.push_back(motion)");
        const size_t replayDimensionGuard =
            replay.find("cache.sidecarSourceW != sourceW");
        const size_t replayHeightGuard =
            replay.find("cache.sidecarSourceH != sourceH");
        CHECK(replaySourceGuard != std::string::npos);
        CHECK(motionXBound != std::string::npos);
        CHECK(motionYBound != std::string::npos);
        CHECK(replayDimensionGuard != std::string::npos);
        CHECK(replayHeightGuard != std::string::npos);
        CHECK(motionXConversion != std::string::npos);
        CHECK(motionYConversion != std::string::npos);
        CHECK(runtimeSourceAssignment != std::string::npos);
        CHECK(motionUpload != std::string::npos);
        // The pre-upload diagnostic observes scaled seeds after every
        // alternate seed path has settled, allowing a dense-field mismatch to
        // be localized without changing the GPU expander input.
        CHECK(playback.find("TFORGE_FSR4_DUMP_MOTION_SEEDS") !=
              std::string::npos);
        const size_t temporalMotionPos =
            playback.find("temporalMotion = scaleMotionCoverageToModel");
        const size_t seedTracePos = playback.find("pre-upload motion seeds");
        const size_t uploadMotionPos =
            playback.find("uploadMotion(\n                  temporalMotion,");
        CHECK(temporalMotionPos != std::string::npos);
        CHECK(seedTracePos != std::string::npos);
        CHECK(uploadMotionPos != std::string::npos);
        if (temporalMotionPos != std::string::npos &&
            seedTracePos != std::string::npos)
            CHECK(temporalMotionPos < seedTracePos);
        if (seedTracePos != std::string::npos &&
            uploadMotionPos != std::string::npos)
            CHECK(seedTracePos < uploadMotionPos);
        // Edge-aware dense reconstruction is a separate opt-in from sparse
        // motion extraction. The typed motion policy must reach the uploader,
        // and the uploader must expose the corresponding shader input without
        // changing the established default when the policy is disabled.
        CHECK(playback.find("temporalMotion, effectiveMotionConfig.edgeAwareUpscale") !=
              std::string::npos);
        CHECK(uploaderHeader.find(
                  "uploadMotion(const std::vector<MvEntry> &mvs,") !=
              std::string::npos);
        CHECK(uploader.find("edgeAware") != std::string::npos);
        CHECK(expand.find("edgeAware") != std::string::npos);
        CHECK(expand.find("sourceColor") != std::string::npos);
        // Prior-jitter metadata belongs to the complete chain, not merely the
        // first pass. A later-pass failure must leave both phase and prior
        // jitter state describing the last fully completed frame.
        const size_t passLoopPos =
            playback.find("for (size_t pass = 1; dr.ok && pass < passCount;");
        const size_t previousJitterAssignment =
            playback.find("previousJitterX = in.jitterX");
        CHECK(passLoopPos != std::string::npos);
        CHECK(previousJitterAssignment != std::string::npos);
        if (passLoopPos != std::string::npos &&
            previousJitterAssignment != std::string::npos)
            CHECK(passLoopPos < previousJitterAssignment);
        if (replaySourceGuard != std::string::npos &&
            runtimeSourceAssignment != std::string::npos)
            CHECK(replaySourceGuard < runtimeSourceAssignment);
        if (motionXBound != std::string::npos &&
            motionXConversion != std::string::npos)
            CHECK(motionXBound < motionXConversion);
        if (motionYBound != std::string::npos &&
            motionYConversion != std::string::npos)
            CHECK(motionYBound < motionYConversion);
        if (motionXBound != std::string::npos &&
            motionUpload != std::string::npos)
            CHECK(motionXBound < motionUpload);
        if (motionYBound != std::string::npos &&
            motionUpload != std::string::npos)
            CHECK(motionYBound < motionUpload);
    }

    CHECK(expand.find("atomicMax(owners[pixelIndex], vectorIndex + 1u)") !=
          std::string::npos);
    CHECK(expand.find("vectors[owner - 1u].motion") != std::string::npos);
    // Edge-aware dense reconstruction must be able to fill a small uncovered
    // boundary hole from a nearby compatible covered block. The fallback is
    // deliberately confidence-reduced and opt-in; an uncovered pixel must not
    // silently become a high-confidence fabricated motion vector.
    CHECK(expand.find("nearest compatible covered vector") !=
          std::string::npos);
    CHECK(expand.find("fallbackConfidence") != std::string::npos);
    CHECK(expand.find("EDGE_AWARE_HOLE_RADIUS") != std::string::npos);
    CHECK(expand.find("imageStore(motionImage") != std::string::npos);
    CHECK(expand.find("layout(binding = 3, r8) writeonly uniform image2D validityImage") !=
          std::string::npos);
    CHECK(expand.find("vectors[owner - 1u].confidence") != std::string::npos);

    // The integrated motion task must expose the actual prepass reprojection
    // result, not just sparse seeds and the dense MV texture. Without this
    // readback, a downstream quality regression cannot distinguish a wrong
    // vector from a history/FSR handoff that ignored a correct vector.
    const std::string uploaderApi =
        readSource("src/render/GpuImageUploader.hpp");
    const std::string uploaderImpl =
        readSource("src/render/GpuImageUploader.cpp");
    const std::string playbackSource =
        readSource("src/core/PlaybackEngine.cpp");
    CHECK(uploaderApi.find("readbackReprojectedColor") != std::string::npos);
    CHECK(uploaderImpl.find("readbackReprojectedColor") != std::string::npos);
    CHECK(playbackSource.find("TFORGE_FSR4_DUMP_REPROJECTED_COLOR") !=
          std::string::npos);
    CHECK(prepass.find("binding = 6, r8") != std::string::npos);
    CHECK(prepass.find("u_motionValidity") != std::string::npos);
    CHECK(prepass.find("motionCovered") != std::string::npos);
    CHECK(prepass.find("correspondenceConfidence") != std::string::npos);
    CHECK(prepass.find("photometricHistoryGate") != std::string::npos);
    CHECK(prepass.find("photometricConfidence") != std::string::npos);
    CHECK(prepass.find("historyLuma") != std::string::npos);
    CHECK(prepass.find("536870912u") != std::string::npos);
    CHECK(prepass.find("8388608u") != std::string::npos);
    CHECK(prepass.find("16777216u") != std::string::npos);
    CHECK(prepass.find("if (motionCovered && onscreen)") != std::string::npos);
    CHECK(prepass.find("slot0.z & 512u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 8192u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 16384u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 32768u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 65536u") != std::string::npos);
    // The reprojection dump has a diagnostic-only current-color arm. It is
    // required to distinguish an empty history sample from a broken FP16
    // image readback; production behavior must remain unchanged unless the
    // explicit environment switch is present.
    CHECK(prepass.find("slot3.x & 1u") != std::string::npos);
    CHECK(harness.find("cbData.s3x") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_DUMP_CURRENT_HISTORY") !=
          std::string::npos);
    CHECK(prepass.find("slot0.z & 262144u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 524288u") != std::string::npos);
    CHECK(prepass.find("slot0.z & 1048576u") != std::string::npos);
    CHECK(prepass.find("sampleHistoryLinear") != std::string::npos);
    CHECK(prepass.find("sampleHistoryNearest") != std::string::npos);
    CHECK(postpass.find("TFORGE_POSTPASS_RECURRENT_RESET_ONLY") !=
          std::string::npos);
    CHECK(postpass.find("imageStore(u_recurrentOut, coord, vec4(0.0))") !=
          std::string::npos);
    // A full scene-cut reset must clear the recurrent ping-pong destination as
    // well as suppressing recurrent input. Otherwise advanceHistory() can
    // expose stale state on the next displayed frame. This assertion is
    // intentionally written before the shader fix so the contract fails if
    // the reset write is removed or remains opt-in-only.
    CHECK(postpass.find("if ((slot0.z & 2u) != 0u || recurrentResetOnly)") !=
          std::string::npos);
    CHECK(uploaderHeader.find("motionValidityView") != std::string::npos);
    // The runtime proof path must be able to inspect the dense textures that
    // the GPU expansion actually produced, not only the sparse JSON seeds.
    CHECK(uploaderHeader.find("readbackMotion") != std::string::npos);
    CHECK(uploaderHeader.find("readbackMotionValidity") != std::string::npos);
    CHECK(uploader.find("readbackMotion(std::vector<uint8_t>") !=
          std::string::npos);
    CHECK(uploader.find("readbackMotionValidity(std::vector<uint8_t>") !=
          std::string::npos);
    CHECK(playback.find("TFORGE_FSR4_DUMP_MOTION_TEXTURE") !=
          std::string::npos);
    CHECK(runner.find("TFORGE_FSR4_DUMP_MOTION_TEXTURE") !=
          std::string::npos);
    CHECK(uploader.find("VK_FORMAT_R8_UNORM") != std::string::npos);
    CHECK(uploader.find("motionValidity_") != std::string::npos);
    CHECK(harnessHeader.find("motionValidityView") != std::string::npos);
    CHECK(harness.find("in.motionValidityView") != std::string::npos);
    CHECK(harness.find("in.motionValidityImage") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_DISABLE_MOTION_VALIDITY") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN") !=
          std::string::npos);
    // FFmpeg's source field carries reference-list identity (negative past,
    // positive future). Collapsing it to a sign hides whether a past vector
    // actually belongs to the immediately preceding frame, which is the only
    // history image this causal player owns. Preserve the original value and
    // expose previous-reference-only filtering as a diagnostic.
    CHECK(playback.find("TFORGE_FSR4_EXPERIMENTAL_PREVIOUS_REFERENCE_ONLY") !=
          std::string::npos);
    CHECK(playback.find("mv.source == -1") != std::string::npos);

    // Regression: only the immediately previous reference may enter the
    // causal history path. FFmpeg documents source as a past/future
    // direction, not a reliable reference age, so source=0 is ambiguous and
    // source=-2 may identify an older past reference. Both must fail closed;
    // source=+1 remains the future-reference rejection control.
    const size_t pastReferenceStart =
        playback.find("std::vector<MvEntry> pastReferenceMotion(");
    const size_t pastReferenceEnd = playback.find(
        "\nbool dumpCausalMotionFrame(", pastReferenceStart);
    CHECK(pastReferenceStart != std::string::npos);
    CHECK(pastReferenceEnd != std::string::npos);
    if (pastReferenceStart != std::string::npos &&
        pastReferenceEnd != std::string::npos) {
        const std::string pastReferenceFilter = playback.substr(
            pastReferenceStart, pastReferenceEnd - pastReferenceStart);
        const int acceptedPreviousSource = -1;
        const int ambiguousZeroSource = 0;
        const int ambiguousOlderSource = -2;
        const int rejectedFutureSource = 1;
        CHECK(acceptedPreviousSource == -1 &&
              pastReferenceFilter.find("mv.source == -1") !=
                  std::string::npos);
        CHECK(ambiguousZeroSource == 0 &&
              pastReferenceFilter.find("mv.source <= 0") ==
                  std::string::npos);
        CHECK(ambiguousOlderSource == -2 &&
              pastReferenceFilter.find("mv.source <= 0") ==
                  std::string::npos);
        CHECK(rejectedFutureSource == 1 &&
              pastReferenceFilter.find("mv.source <= 0") ==
                  std::string::npos);
    }
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING") !=
          std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION") !=
          std::string::npos);
    // AMD's video integration contract keeps motion vectors unjittered. The
    // prepass must therefore be able to sample motion at the unjittered source
    // coordinate while retaining the jittered color sample. This is an
    // opt-in diagnostic until a matched real-scene A/B proves it helps.
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_UNJITTERED_MOTION_SAMPLE") !=
          std::string::npos);
    CHECK(prepass.find("TFORGE_PREPASS_UNJITTERED_MOTION_SAMPLE") !=
          std::string::npos);
    CHECK(prepass.find("const vec2 unjitteredInputPos") !=
          std::string::npos);
    CHECK(prepass.find("vec2 inputPos = unjitteredInputPos") !=
          std::string::npos);
    CHECK(prepass.find("vec2 motionSamplePos = unjitteredInputPos") !=
          std::string::npos);
    // The default motion lookup is unjittered. A separate, explicitly named
    // diagnostic must be able to reintroduce the color jitter so a matched A/B
    // can prove that lookup choice matters; the older unjittered flag alone is
    // redundant once inputPos is already unshifted.
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_JITTERED_MOTION_SAMPLE") !=
          std::string::npos);
    CHECK(prepass.find("TFORGE_PREPASS_JITTERED_MOTION_SAMPLE") !=
          std::string::npos);
    CHECK(prepass.find("motionSamplePos += slot1.zw") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY") !=
          std::string::npos);

    CHECK(harness.find("in.historyConfidence") != std::string::npos);
    CHECK(harness.find("in.reset ? 1u : 0u") != std::string::npos);
    CHECK(playback.find("historyConfidence = sideInputs.motionConfidence") !=
          std::string::npos);

    // Jittered video frames need the current-minus-previous jitter delta when
    // sampling the previously published history image. Without that delta,
    // static content is sampled at the wrong subpixel phase on every frame.
    // Upstream: SideBufferSynth's current jitter and the last successfully
    // dispatched frame. Downstream: prepass history/recurrent reprojection.
    CHECK(harnessHeader.find("previousJitterX") != std::string::npos);
    CHECK(harnessHeader.find("previousJitterY") != std::string::npos);
    CHECK(playback.find("previousJitterX") != std::string::npos);
    CHECK(playback.find("previousJitterY") != std::string::npos);
    CHECK(harness.find("TFORGE_FSR4_EXPERIMENTAL_HISTORY_JITTER_DELTA") !=
          std::string::npos);
    CHECK(harness.find("cbData.s2z = in.jitterX - in.previousJitterX") !=
          std::string::npos);
    CHECK(harness.find("cbData.s2w = in.jitterY - in.previousJitterY") !=
          std::string::npos);
    CHECK(harness.find("in.jitterX != 0.0f || in.jitterY != 0.0f") !=
          std::string::npos);
    CHECK(prepass.find("slot2.zw") != std::string::npos);
    CHECK(prepass.find("historyJitterDelta") != std::string::npos);
    // The jitter offset is authored in source/render pixels, while history
    // is stored at output resolution. Reprojection must therefore apply the
    // same source-to-output scale used for motion before adding the delta.
    CHECK(prepass.find("historyJitterDelta =") != std::string::npos);
    CHECK(prepass.find("slot2.zw * modelToOutputScale") != std::string::npos);
    CHECK(prepass.find("historyPos = vec2(px, py) + motion +") !=
          std::string::npos);

    // The exported motion sidecar must describe the exact post-filter field
    // consumed by the uploader. Upstream: codec/replacement/refinement/
    // validation/confidence stages. Downstream: reproducible coverage and
    // temporal-metric evidence. Dumping the raw decoder vectors here would
    // silently mislabel an opt-in capture's actual motion input.
    const size_t motionDump = playback.find("dumpCausalMotionFrame(");
    CHECK(motionDump != std::string::npos);
    CHECK(playback.find("sideInputs.motionConfidence,\n                          pastMotion",
                       motionDump) != std::string::npos);
    CHECK(playback.find("sideInputs.motionConfidence,\n                          pastReferenceMotion(fsrFrame->motionVectors)",
                       motionDump) == std::string::npos);
    CHECK(playback.find("motion.confidence", motionDump) !=
          std::string::npos);

    // Hardware decode keeps the zero-copy DRM surface for presentation, but
    // the quality campaign may opt in to a separate software analysis copy so
    // the causal motion matcher has real luma instead of silently receiving
    // an empty buffer. The opt-in must remain explicit until its performance
    // and quality are measured against the hardware-only baseline.
    CHECK(decoder.find("TFORGE_FSR4_ENABLE_HW_ANALYSIS_LUMA") !=
          std::string::npos);
    // Explicit block-motion/replacement captures must request the analysis
    // bridge themselves when hardware decode is active; otherwise the named
    // motion option silently receives no luma and produces no vectors.
    CHECK(decoder.find("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION") !=
          std::string::npos);
    CHECK(decoder.find("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION") !=
          std::string::npos);
    CHECK(decoder.find("av_hwframe_transfer_data(analysisFrame") !=
          std::string::npos);
    CHECK(decoder.find("out.plane[0]") != std::string::npos);

    // A detected scene cut must take the reset-safe upload path and reach the
    // postpass reset bit. Upstream: SideBufferSynth/PlaybackEngine reset
    // detection. Downstream: temporal history invalidation in postpass; a
    // regression here can leave stale native-path history after a cut.
    CHECK(playback.find("const bool initializeNeutral =") !=
          std::string::npos);
    CHECK(harness.find("pp.slot0[2] = recurrentResetOnly ? 268435456u") !=
          std::string::npos);

    // The explicit disable switch must bypass the integrated adaptive
    // multiplier too. Otherwise a supposedly ungated A/B still gets
    // confidence-suppressed later in the same dispatch.
    CHECK(harness.find("if (!disableLearnedConfidenceGate &&") !=
          std::string::npos);

    if (g_failures == 0) {
        std::printf("fsr4_motion_contract_tests: OK\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_motion_contract_tests: %d FAILURES\n", g_failures);
    return 1;
}
