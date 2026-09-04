// PlaybackEngine.cpp
#include "core/PlaybackEngine.hpp"
#include "backend/GpuCapabilityProbe.hpp"
#include "backend/WeightBlob.hpp"
#include "util/FsrTargetMath.hpp"
#include "util/Log.hpp"
#include "util/TemporalFrameContinuity.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <utility>
namespace temporal_forge {

using namespace std::chrono_literals;

namespace {

// diagnosticFingerprint: produce a stable, compact fingerprint for a GPU
// readback used by an opt-in provenance check. Upstream: the EASU/downscale
// diagnostic buffers. Downstream: capture logs that prove whether the
// pre-neural candidate actually changed the image handed to FSR4. This is
// diagnostic-only and never participates in reconstruction decisions.
uint64_t diagnosticFingerprint(const std::vector<uint8_t> &bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool dumpPackedRgb10Ppm(const std::filesystem::path &path,
                        const std::vector<uint8_t> &bytes, uint32_t width,
                        uint32_t height) {
  if (bytes.size() != static_cast<size_t>(width) * height * sizeof(uint32_t))
    return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << "P6\n" << width << ' ' << height << "\n255\n";
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    uint32_t packed = 0;
    std::memcpy(&packed, bytes.data() + i * sizeof(uint32_t), sizeof(packed));
    const std::array<uint8_t, 3> rgb = {
        static_cast<uint8_t>(((packed >> 0u) & 1023u) * 255u / 1023u),
        static_cast<uint8_t>(((packed >> 10u) & 1023u) * 255u / 1023u),
        static_cast<uint8_t>(((packed >> 20u) & 1023u) * 255u / 1023u)};
    out.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
  }
  return static_cast<bool>(out);
}

std::pair<uint32_t, uint32_t> fsrViewportForBenchmark(uint32_t fallbackW,
                                                       uint32_t fallbackH) {
  const char *value = std::getenv("TFORGE_FSR4_FORCE_VIEWPORT");
  if (!value || !*value)
    return {fallbackW, fallbackH};
  unsigned int width = 0, height = 0;
  char tail = '\0';
  if (std::sscanf(value, "%ux%u%c", &width, &height, &tail) == 2 &&
      width >= 2 && height >= 2)
    return {width, height};
  logWarn("PlaybackEngine: invalid TFORGE_FSR4_FORCE_VIEWPORT '{}'; using {}x{}",
          value, fallbackW, fallbackH);
  return {fallbackW, fallbackH};
}

// FSR jitter phase selection depends on the actual model/output pair for the
// frame being submitted.  Keep this sizing calculation in one place so the
// pair is installed before SideBufferSynth chooses the variable-jitter phase,
// while the dispatch path reuses the exact same dimensions afterward.
struct FsrJitterPair {
  uint32_t modelW = 0;
  uint32_t modelH = 0;
  uint32_t neuralTargetW = 0;
  uint32_t neuralTargetH = 0;
  uint32_t displayW = 0;
  uint32_t displayH = 0;
  bool nativePassthrough = false;
  // Explicit FSR1/EASU prefilter request. This is independent of the FSR4
  // model dimensions: EASU always writes its 2x-native intermediate, then the
  // existing RGB10 downscale hands that result to the model-sized color image.
  bool preEasu = false;
};

FsrJitterPair computeFsrJitterPair(uint32_t decodedW, uint32_t decodedH,
                                   float selectedScale, bool forcedViewport,
                                   uint32_t viewportW, uint32_t viewportH,
                                   uint32_t targetW, uint32_t targetH) {
  const Size2D nativeTarget = nativeInt8FixedTarget(
      alignEven(decodedW), alignEven(decodedH));
  FsrJitterPair pair;
  pair.neuralTargetW = nativeTarget.width;
  pair.neuralTargetH = nativeTarget.height;
  pair.displayW = std::max(2u, viewportW);
  pair.displayH = std::max(2u, viewportH);

  const auto fitToViewport = [&](uint32_t fitViewportW,
                                 uint32_t fitViewportH) {
    const double fit = std::min(
        static_cast<double>(fitViewportW) / decodedW,
        static_cast<double>(fitViewportH) / decodedH);
    return std::pair<uint32_t, uint32_t>{
        std::max(2u, alignEven(static_cast<uint32_t>(
                                   std::round(decodedW * fit)))),
        std::max(2u, alignEven(static_cast<uint32_t>(
                                   std::round(decodedH * fit))))};
  };

  if (forcedViewport) {
    const auto benchmarkViewport = fsrViewportForBenchmark(
        std::max(2u, targetW), std::max(2u, targetH));
    if (nativeTarget.width == benchmarkViewport.first &&
        nativeTarget.height == benchmarkViewport.second) {
      pair.neuralTargetW = nativeTarget.width;
      pair.neuralTargetH = nativeTarget.height;
    } else {
      const auto fitted = fitToViewport(benchmarkViewport.first,
                                        benchmarkViewport.second);
      pair.neuralTargetW = fitted.first;
      pair.neuralTargetH = fitted.second;
    }
    pair.displayW = pair.neuralTargetW;
    pair.displayH = pair.neuralTargetH;
  } else {
    const auto fitted = fitToViewport(pair.displayW, pair.displayH);
    pair.displayW = fitted.first;
    pair.displayH = fitted.second;
  }

  const uint32_t fsrInputW = std::max(
      2u, alignEven(static_cast<uint32_t>(std::round(
              pair.neuralTargetW / std::max(1.0f, selectedScale)))));
  const uint32_t fsrInputH = std::max(
      2u, alignEven(static_cast<uint32_t>(std::round(
              pair.neuralTargetH / std::max(1.0f, selectedScale)))));
  // The selected multiplier defines the reconstruction grid. Do not clamp it
  // to the decoded frame: supersampling arms intentionally reconstruct from
  // a model-sized input larger than the decoded source before presentation
  // downsampling. The uploader keeps decoded and model dimensions separate.
  const uint32_t modelW = fsrInputW;
  const uint32_t modelH = fsrInputH;
  pair.nativePassthrough =
      pair.neuralTargetW == decodedW && pair.neuralTargetH == decodedH &&
      pair.displayW == decodedW && pair.displayH == decodedH;
  pair.preEasu =
      (std::getenv("TFORGE_FSR4_PRE_EASU") != nullptr ||
       std::getenv("TFORGE_FSR4_TRUE_FSR1_EASU") != nullptr) &&
      !pair.nativePassthrough && decodedW > 0 && decodedH > 0;
  pair.modelW = modelW;
  pair.modelH = modelH;
  return pair;
}

void writeRuntimePipelineTrace(uint32_t decodedW, uint32_t decodedH,
                               uint32_t modelW, uint32_t modelH,
                               uint32_t outputW, uint32_t outputH,
                               float selectedScale, bool nativeInt8,
                               size_t passCount) {
  const char *tracePath = std::getenv("TFORGE_RUNTIME_TRACE_PATH");
  if (!tracePath || !*tracePath)
    return;

  const char *jitterEnv = std::getenv("TFORGE_FSR4_JITTER_MODE");
  const bool jitterOff = jitterEnv && std::strcmp(jitterEnv, "off") == 0;
  const bool prepassJitter =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_PREPASS_JITTER_ORDERING") ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER");
  const bool historyEnabled =
      std::getenv("TFORGE_FSR4_ENABLE_COLOR_HISTORY") ||
      std::getenv("TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE") ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS") ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER");
  const char *casEnv = std::getenv("TFORGE_FSR4_CAS_STRENGTH");
  const float casStrength = casEnv && *casEnv
                                ? std::clamp(std::strtof(casEnv, nullptr), 0.0f, 1.0f)
                                : 0.20f;
  const bool casEnabled = std::getenv("TFORGE_FSR4_DISABLE_CAS") == nullptr;

  QJsonObject trace;
  trace["schema"] = "temporal_forge.runtime_pipeline.v1";
  trace["quality_profile"] = QString::fromUtf8(
      std::getenv("TFORGE_QUALITY_PROFILE") ? std::getenv("TFORGE_QUALITY_PROFILE") : "");
  trace["config_sha256"] = QString::fromUtf8(
      std::getenv("TFORGE_CONFIG_SHA256") ? std::getenv("TFORGE_CONFIG_SHA256") : "");
  trace["run_id"] = QString::fromUtf8(
      std::getenv("TFORGE_EXPERIMENT_ID") ? std::getenv("TFORGE_EXPERIMENT_ID") : "");
  trace["binary_path"] = QString::fromUtf8("/proc/self/exe");
  QFile executable(QStringLiteral("/proc/self/exe"));
  if (executable.open(QIODevice::ReadOnly))
    trace["binary_sha256"] = QString::fromLatin1(
        QCryptographicHash::hash(executable.readAll(), QCryptographicHash::Sha256).toHex());
  trace["git_head"] = QString::fromUtf8(
      std::getenv("TFORGE_GIT_HEAD") ? std::getenv("TFORGE_GIT_HEAD") : "");
  trace["git_dirty"] = std::getenv("TFORGE_GIT_DIRTY") == nullptr
                            ? QJsonValue()
                            : QJsonValue(std::strcmp(std::getenv("TFORGE_GIT_DIRTY"), "1") == 0);
  trace["backend"] = nativeInt8 ? "native_int8" : "generic_fsr4";
  trace["graph_passes"] = static_cast<int>(passCount);
  trace["source_resolution"] =
      QStringLiteral("%1x%2").arg(decodedW).arg(decodedH);
  trace["reconstruction_resolution"] =
      QStringLiteral("%1x%2").arg(modelW).arg(modelH);
  trace["presentation_resolution"] =
      QStringLiteral("%1x%2").arg(outputW).arg(outputH);
  trace["requested_scale"] = selectedScale;
  const uint32_t nominalModelW = std::max(
      2u, alignEven(static_cast<uint32_t>(std::round(
              outputW / std::max(1.0f, selectedScale)))));
  const uint32_t nominalModelH = std::max(
      2u, alignEven(static_cast<uint32_t>(std::round(
              outputH / std::max(1.0f, selectedScale)))));
  trace["requested_model_resolution"] =
      QStringLiteral("%1x%2").arg(nominalModelW).arg(nominalModelH);
  trace["scale_clamped_to_source"] =
      modelW < nominalModelW || modelH < nominalModelH;
  trace["effective_scale"] = modelW > 0
                                  ? static_cast<double>(outputW) / modelW
                                  : 0.0;
  trace["jitter_mode"] = jitterOff ? "off" :
                         (prepassJitter ? "prepass_input_resolve" : "synthetic_upload");
  trace["jitter_enabled"] = !jitterOff;
  trace["motion_lookup"] = "unjittered_source_coordinates";
  // Motion is expanded into the model-sized RG16F/R8 pair before the FSR
  // prepass. Keep the domain and transform explicit: a source-space vector
  // must never be interpreted as a model-texture coordinate without this
  // scale.
  trace["motion_texture_resolution"] =
      QStringLiteral("%1x%2").arg(modelW).arg(modelH);
  trace["source_to_motion_scale_x"] = decodedW > 0
                                           ? static_cast<double>(modelW) / decodedW
                                           : 0.0;
  trace["source_to_motion_scale_y"] = decodedH > 0
                                           ? static_cast<double>(modelH) / decodedH
                                           : 0.0;
  trace["motion_units"] = "source_pixels";
  trace["motion_direction"] = "current_to_previous";
  trace["motion_coordinate_convention"] =
      "current_destination_plus_motion_previous_reference";
  trace["motion_sample_domain"] = "unjittered_source_coordinates";
  trace["motion_source"] =
      std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR") ||
              std::getenv("TFORGE_FSR4_MOTION_ABLATION")
          ? "configured_estimator_or_ablation"
          : "decoder_motion_or_default";
  trace["motion_validity_enabled"] =
      std::getenv("TFORGE_FSR4_DISABLE_MOTION_VALIDITY") == nullptr;
  trace["motion_validity_distinct_from_zero"] = true;
  trace["invalid_history_policy"] =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_CURRENT_INVALID_HISTORY")
          ? "current_color_compatibility_diagnostic"
          : "reject_invalid_history";
  trace["history_enabled"] = historyEnabled;
  trace["recurrent_enabled"] =
      std::getenv("TFORGE_FSR4_ENABLE_RECURRENT") != nullptr;
  trace["history_reset_policy"] = "seek_resize_cut_or_invalid_correspondence";
  trace["cas_enabled"] = casEnabled;
  trace["cas_strength"] = casStrength;
  trace["cas_stage"] = casEnabled ? "integrated_post_reconstruction" : "none";
  trace["post_fsr_reducer"] = "none";
  trace["presentation_scaler"] = "runtime_display_scaler_or_native_output";
  trace["jitter_sequence"] = QString::fromUtf8(
      std::getenv("TFORGE_FSR4_JITTER_SEQUENCE")
          ? std::getenv("TFORGE_FSR4_JITTER_SEQUENCE")
          : "halton23");
  trace["jitter_phase_source"] = "per_frame_event_trace";
  // The legacy source-tap switch is a requested/profile-selection value.  It
  // must not be reported as though the prepass applies Mu-law to rgba8 taps:
  // the production resolve samples the transformed model-color image, and
  // the transform is performed upstream by yuv_to_fsr_input.
  trace["requested_source_tap_mulaw_profile"] =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") != nullptr;
  trace["prepass_resolve_source"] = "model_color";
  trace["prepass_resolve_stage"] = "prepass_input_resolve";
  trace["prepass_resolve_resolution"] =
      QStringLiteral("%1x%2").arg(outputW).arg(outputH);
  trace["model_color_transfer"] = "eotf_mulaw_pretransformed";
  trace["model_color_format"] = "rgb10_a2";
  trace["model_color_resolution"] =
      QStringLiteral("%1x%2").arg(modelW).arg(modelH);
  trace["mulaw_application_stage"] = "yuv_to_model_color";
  trace["mulaw_sampling_semantics"] = "pretransformed_before_resolve";
  trace["jitter_stage"] = !jitterOff && prepassJitter
                              ? "prepass_input_resolve"
                              : (jitterOff ? "none" : "upload");
  trace["source_display_format"] = "rgba8";
  trace["source_display_resolution"] =
      QStringLiteral("%1x%2").arg(decodedW).arg(decodedH);
  trace["source_display_used_for_current_resolve"] = false;
  trace["requested_force_viewport"] = QString::fromUtf8(
      std::getenv("TFORGE_FSR4_FORCE_VIEWPORT") ? std::getenv("TFORGE_FSR4_FORCE_VIEWPORT") : "");
  trace["requested_force_scale"] = QString::fromUtf8(
      std::getenv("TFORGE_FSR4_FORCE_SCALE") ? std::getenv("TFORGE_FSR4_FORCE_SCALE") : "");

  QFile file(QString::fromLocal8Bit(tracePath));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    logWarn("PlaybackEngine: cannot write runtime pipeline trace '{}': {}",
            tracePath, file.errorString().toStdString());
    return;
  }
  const QByteArray payload = QJsonDocument(trace).toJson(QJsonDocument::Compact);
  if (file.write(payload) != payload.size())
    logWarn("PlaybackEngine: short write for runtime pipeline trace '{}'", tracePath);
  file.close();
}

int qualityLabPresentationScaler(const QualityLabConfig &config,
                                 int fallback) {
  if (!config.enabled)
    return fallback;
  switch (config.presentationFilter) {
  case QualityPresentationFilter::Nearest: return 0;
  case QualityPresentationFilter::Linear: return 1;
  case QualityPresentationFilter::Bicubic: return 2;
  case QualityPresentationFilter::Lanczos: return 3;
  }
  return fallback;
}

uint32_t motionAnalysisWidth() {
  constexpr uint32_t defaultWidth = 96;
  const char *value =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_MOTION_ANALYSIS_WIDTH");
  if (!value || !*value)
    return defaultWidth;
  char *end = nullptr;
  const unsigned long raw = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || raw > 384ul)
    return defaultWidth;
  const uint32_t parsed = static_cast<uint32_t>(raw);
  return std::clamp(parsed, 32u, 384u);
}

bool motionEstimatorRequested() {
  // The capture runner has two equivalent ways to select the standalone
  // estimator: the dedicated estimator variable and the human-readable
  // motion ablation label. Keep this predicate aligned with
  // MotionEstimator::configFromEnvironment so both the analysis-grid choice
  // and the estimator mode describe the same work. Upstream: benchmark/runtime
  // selection. Downstream: makeAnalysisLuma's source evidence resolution.
  const char *mode = std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR");
  if (mode && *mode)
    return std::strcmp(mode, "codec") == 0 ||
           std::strcmp(mode, "codec_refined") == 0 ||
           std::strcmp(mode, "refined") == 0;
  mode = std::getenv("TFORGE_FSR4_MOTION_ABLATION");
  return mode && (std::strcmp(mode, "codec") == 0 ||
                  std::strcmp(mode, "codec_refined") == 0 ||
                  std::strcmp(mode, "refined") == 0);
}

LumaBuffer makeAnalysisLuma(const DecodedVideoFrame &frame,
                           bool estimatorRequested = false) {
  LumaBuffer out;
  if (frame.planes <= 0 || frame.plane[0].empty() || frame.width <= 0 ||
      frame.height <= 0 || frame.linesize[0] <= 0)
    return out;
  // The default is deliberately unchanged. The bounded opt-in lets motion
  // diagnostics use more source evidence without changing decoded pixels or
  // making normal playback pay for a denser CPU analysis grid.
  uint32_t analysisWidth = motionAnalysisWidth();
  // The standalone estimator's documented 1/2, 1/4, and 1/8 controls select
  // the actual luma grid. The legacy path keeps its established compact
  // 96-pixel analysis image when the estimator is not selected.
  if (estimatorRequested || motionEstimatorRequested()) {
    uint32_t divisor = 4u;
    if (const char *value = std::getenv("TFORGE_FSR4_MOTION_REFINEMENT_SCALE")) {
      const long parsed = std::strtol(value, nullptr, 10);
      divisor = parsed <= 2 ? 2u : parsed >= 8 ? 8u : 4u;
    }
    analysisWidth = std::max<uint32_t>(32u,
        static_cast<uint32_t>(std::max(1, frame.width) / divisor));
  }
  out.width = std::min<uint32_t>(analysisWidth,
                                 static_cast<uint32_t>(frame.width));
  out.height = std::max<uint32_t>(1u, static_cast<uint32_t>(
      std::llround(static_cast<double>(frame.height) * out.width / frame.width)));
  out.data.resize(static_cast<size_t>(out.width) * out.height);
  const bool limited = frame.colorRange != AVCOL_RANGE_JPEG;
  // Software FFmpeg planes for YUV420P10LE/YUV420P12LE contain one
  // little-endian sample in two bytes. Analysis luma is upstream of motion
  // refinement, validation, confidence, scene-cut detection, and jitter
  // policy, so reading a high-bit-depth plane as bytes silently corrupts all
  // of those downstream decisions. Keep the 8-bit path numerically intact.
  const bool highBitDepth = frame.bitDepth > 8;
  const size_t bytesPerSample = highBitDepth ? 2u : 1u;
  const uint32_t analysisBitDepth = std::clamp(frame.bitDepth, 8, 16);
  const uint32_t codeMax = (1u << analysisBitDepth) - 1u;
  const uint32_t limitedBiasCode = 16u << (analysisBitDepth - 8u);
  const uint32_t limitedRangeCode = 219u << (analysisBitDepth - 8u);
  const float scale = highBitDepth
      ? 1.0f / static_cast<float>(limited ? limitedRangeCode : codeMax)
      : (limited ? (1.0f / 219.0f) : (1.0f / 255.0f));
  const float bias = highBitDepth
      ? static_cast<float>(limited ? limitedBiasCode : 0u)
      : (limited ? 16.0f : 0.0f);
  for (uint32_t y = 0; y < out.height; ++y) {
    const int sy = std::min(frame.height - 1,
                            static_cast<int>((static_cast<uint64_t>(y) * frame.height) /
                                             out.height));
    for (uint32_t x = 0; x < out.width; ++x) {
      const int sx = std::min(frame.width - 1,
                              static_cast<int>((static_cast<uint64_t>(x) * frame.width) /
                                               out.width));
      const size_t byteOffset = static_cast<size_t>(sy) * frame.linesize[0] +
                                 static_cast<size_t>(sx) * bytesPerSample;
      float yValue = 0.0f;
      if (byteOffset < frame.plane[0].size() &&
          (!highBitDepth || byteOffset + 1u < frame.plane[0].size())) {
        if (highBitDepth) {
          const uint32_t sample =
              static_cast<uint32_t>(frame.plane[0][byteOffset]) |
              (static_cast<uint32_t>(frame.plane[0][byteOffset + 1u]) << 8u);
          yValue = static_cast<float>(sample);
        } else {
          yValue = static_cast<float>(frame.plane[0][byteOffset]);
        }
      }
      out.data[static_cast<size_t>(y) * out.width + x] =
          std::clamp((yValue - bias) * scale, 0.0f, 1.0f);
    }
  }
  return out;
}

// makeMidpointFrame: build a deliberately small, diagnostic-only midpoint
// frame from two adjacent software-decoded frames. This is not motion-
// compensated interpolation: it is the falsification control for the
// hypothesis that future pixels were previously used as evidence but never
// became the image sent through FSR or the image published to the renderer.
//
// Upstream: the current decoded frame and the one-frame lookahead held by the
// decode loop. Downstream: only the opt-in display-interpolation probe, which
// feeds this frame to the existing FSR1/FSR4 path and gives it a midpoint PTS.
// The default path never calls this function. Rejecting hardware surfaces and
// non-8-bit/mismatched frames keeps the probe honest instead of blending
// incomplete or differently encoded buffers.
bool makeMidpointFrame(const DecodedVideoFrame &current,
                       const DecodedVideoFrame &next,
                       DecodedVideoFrame &out) {
  if (current.width <= 0 || current.height <= 0 ||
      current.width != next.width || current.height != next.height ||
      current.avFormat != next.avFormat || current.bitDepth != 8 ||
      next.bitDepth != 8 || current.hwFrame || next.hwFrame ||
      current.planes <= 0 || current.planes != next.planes)
    return false;
  for (int plane = 0; plane < current.planes; ++plane) {
    if (current.plane[plane].size() != next.plane[plane].size() ||
        current.plane[plane].empty())
      return false;
  }

  out = current;
  out.ptsUs = current.ptsUs +
              (next.ptsUs > current.ptsUs
                   ? (next.ptsUs - current.ptsUs) / 2
                   : current.durationUs / 2);
  out.durationUs = next.ptsUs > current.ptsUs
                       ? next.ptsUs - current.ptsUs
                       : current.durationUs;
  // A midpoint has no independently decoded codec-vector side data. Keep the
  // current causal field for this controlled probe; the separate motion
  // correctness experiments remain responsible for validating correspondence.
  out.motionVectors = current.motionVectors;
  for (int plane = 0; plane < current.planes; ++plane) {
    auto &dst = out.plane[plane];
    const auto &src = next.plane[plane];
    for (size_t i = 0; i < dst.size(); ++i)
      dst[i] = static_cast<uint8_t>((static_cast<unsigned>(dst[i]) +
                                     static_cast<unsigned>(src[i]) + 1u) /
                                    2u);
  }
  return true;
}

// makeFutureAlignedFrame: pull the future decoded sample back onto the
// current frame's coordinates, then average the aligned samples. Unlike a
// midpoint, this preserves the current presentation time and is therefore a
// candidate for replacing synthetic jitter rather than inserting a new video
// frame. The field is future->current and is required; without it, averaging
// unrelated pixels would manufacture motion blur.
bool makeFutureAlignedFrame(const DecodedVideoFrame &current,
                            const DecodedVideoFrame &next,
                            const std::vector<MvEntry> &futureToCurrent,
                            DecodedVideoFrame &out,
                            float photometricThreshold = -1.0f) {
  if (current.width <= 0 || current.height <= 0 ||
      current.width != next.width || current.height != next.height ||
      current.avFormat != next.avFormat || current.bitDepth != 8 ||
      next.bitDepth != 8 || current.hwFrame || next.hwFrame ||
      current.planes <= 0 || current.planes != next.planes ||
      futureToCurrent.empty())
    return false;
  for (int plane = 0; plane < current.planes; ++plane) {
    if (current.plane[plane].size() != next.plane[plane].size() ||
        current.plane[plane].empty() || current.linesize[plane] <= 0 ||
        next.linesize[plane] != current.linesize[plane])
      return false;
  }

  out = current;
  out.ptsUs = current.ptsUs;
  out.durationUs = current.durationUs;
  out.motionVectors = current.motionVectors;

  std::vector<int16_t> motionX(static_cast<size_t>(current.width) *
                               current.height, 0);
  std::vector<int16_t> motionY(motionX.size(), 0);
  // A zero entry means the warped future sample disagreed with the current
  // luma at this pixel and must not contaminate the displayed sample. The
  // mask is computed once on luma and reused for chroma, so all planes share
  // one disocclusion decision.
  std::vector<uint8_t> photometricBlend(motionX.size(), 1u);
  for (const MvEntry &mv : futureToCurrent) {
    const int x0 = std::clamp(static_cast<int>(mv.dstX), 0, current.width);
    const int y0 = std::clamp(static_cast<int>(mv.dstY), 0, current.height);
    const int x1 = std::clamp(static_cast<int>(mv.dstX) +
                                  std::max(1, static_cast<int>(mv.w)),
                              0, current.width);
    const int y1 = std::clamp(static_cast<int>(mv.dstY) +
                                  std::max(1, static_cast<int>(mv.h)),
                              0, current.height);
    const int dx = std::clamp(static_cast<int>(std::lround(mv.mvX)),
                              -32768, 32767);
    const int dy = std::clamp(static_cast<int>(std::lround(mv.mvY)),
                              -32768, 32767);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const size_t index = static_cast<size_t>(y) * current.width + x;
        motionX[index] = static_cast<int16_t>(dx);
        motionY[index] = static_cast<int16_t>(dy);
      }
  }

  for (int plane = 0; plane < current.planes; ++plane) {
    const bool chroma = plane > 0 && current.planes >= 3;
    const int planeWidth = chroma ? (current.width + 1) / 2 : current.width;
    const int planeHeight = chroma ? (current.height + 1) / 2 : current.height;
    auto &dst = out.plane[plane];
    const auto &a = current.plane[plane];
    const auto &b = next.plane[plane];
    for (int y = 0; y < planeHeight; ++y) {
      for (int x = 0; x < planeWidth; ++x) {
        const int fullX = std::min(current.width - 1,
                                   x * (chroma ? 2 : 1));
        const int fullY = std::min(current.height - 1,
                                   y * (chroma ? 2 : 1));
        const size_t motionIndex = static_cast<size_t>(fullY) * current.width +
                                   fullX;
        const int dx = motionX[motionIndex];
        const int dy = motionY[motionIndex];
        // future->current motion maps a future sample at p to current p+mv.
        // Therefore the future source for current coordinate p is p-mv.
        const int bx = std::clamp(x - (chroma ? dx / 2 : dx), 0,
                                  planeWidth - 1);
        const int by = std::clamp(y - (chroma ? dy / 2 : dy), 0,
                                  planeHeight - 1);
        const unsigned av = a[static_cast<size_t>(y) * current.linesize[plane] +
                              x];
        const unsigned bv = b[static_cast<size_t>(by) * next.linesize[plane] +
                              bx];
        bool blendFuture = photometricThreshold < 0.0f;
        if (plane == 0 && photometricThreshold >= 0.0f) {
          const size_t fullIndex = static_cast<size_t>(fullY) * current.width +
                                   fullX;
          blendFuture = std::abs(static_cast<int>(av) - static_cast<int>(bv)) /
                            255.0f <=
                        photometricThreshold;
          photometricBlend[fullIndex] = blendFuture ? 1u : 0u;
        } else if (plane > 0 && photometricThreshold >= 0.0f) {
          const size_t fullIndex = static_cast<size_t>(fullY) * current.width +
                                   fullX;
          blendFuture = photometricBlend[fullIndex] != 0u;
        }
        dst[static_cast<size_t>(y) * current.linesize[plane] + x] =
            blendFuture ? static_cast<uint8_t>((av + bv + 1u) / 2u)
                        : static_cast<uint8_t>(av);
      }
    }
  }
  return true;
}

// makeMotionCompensatedMidpointFrame: synthesize a midpoint while moving the
// two source samples toward one another according to future->current motion.
// The vector field is intentionally supplied by the caller so this helper can
// be paired with the same correspondence experiment being evaluated. It is a
// CPU diagnostic path for software-decoded 8-bit frames, not a production
// interpolation algorithm: uncovered blocks use zero motion and all samples
// use nearest source pixels. The output is nevertheless a real intermediate
// frame that is sent through FSR and published when the explicit probe is on.
//
// Upstream: adjacent decoded YUV frames and validated/diagnostic block motion.
// Downstream: FSR1/FSR4 color upload, temporal dispatch, and renderer timing.
// Default playback never calls this function.
bool makeMotionCompensatedMidpointFrame(
    const DecodedVideoFrame &current, const DecodedVideoFrame &next,
    const std::vector<MvEntry> &futureToCurrent, DecodedVideoFrame &out) {
  if (current.width <= 0 || current.height <= 0 ||
      current.width != next.width || current.height != next.height ||
      current.avFormat != next.avFormat || current.bitDepth != 8 ||
      next.bitDepth != 8 || current.hwFrame || next.hwFrame ||
      current.planes <= 0 || current.planes != next.planes ||
      futureToCurrent.empty())
    return false;
  for (int plane = 0; plane < current.planes; ++plane) {
    if (current.plane[plane].size() != next.plane[plane].size() ||
        current.plane[plane].empty() || current.linesize[plane] <= 0 ||
        next.linesize[plane] != current.linesize[plane])
      return false;
  }

  out = current;
  out.ptsUs = current.ptsUs +
              (next.ptsUs > current.ptsUs
                   ? (next.ptsUs - current.ptsUs) / 2
                   : current.durationUs / 2);
  out.durationUs = next.ptsUs > current.ptsUs
                       ? next.ptsUs - current.ptsUs
                       : current.durationUs;
  out.motionVectors = current.motionVectors;

  // Expand sparse source-space vectors into a small integer field. The loop is
  // bounded by the decoded frame, and later entries retain the same
  // deterministic last-writer behavior as the GPU sparse expansion.
  std::vector<int16_t> motionX(static_cast<size_t>(current.width) *
                               current.height, 0);
  std::vector<int16_t> motionY(motionX.size(), 0);
  for (const MvEntry &mv : futureToCurrent) {
    const int x0 = std::clamp(static_cast<int>(mv.dstX), 0, current.width);
    const int y0 = std::clamp(static_cast<int>(mv.dstY), 0, current.height);
    const int x1 = std::clamp(static_cast<int>(mv.dstX) + std::max(1, (int)mv.w),
                              0, current.width);
    const int y1 = std::clamp(static_cast<int>(mv.dstY) + std::max(1, (int)mv.h),
                              0, current.height);
    const int dx = std::clamp(static_cast<int>(std::lround(mv.mvX)), -32768, 32767);
    const int dy = std::clamp(static_cast<int>(std::lround(mv.mvY)), -32768, 32767);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const size_t index = static_cast<size_t>(y) * current.width + x;
        motionX[index] = static_cast<int16_t>(dx);
        motionY[index] = static_cast<int16_t>(dy);
      }
  }

  for (int plane = 0; plane < current.planes; ++plane) {
    const bool chroma = plane > 0 && current.planes >= 3;
    const int planeWidth = chroma ? (current.width + 1) / 2 : current.width;
    const int planeHeight = chroma ? (current.height + 1) / 2 : current.height;
    auto &dst = out.plane[plane];
    const auto &a = current.plane[plane];
    const auto &b = next.plane[plane];
    for (int y = 0; y < planeHeight; ++y) {
      for (int x = 0; x < planeWidth; ++x) {
        const int fullX = std::min(current.width - 1, x * (chroma ? 2 : 1));
        const int fullY = std::min(current.height - 1, y * (chroma ? 2 : 1));
        const size_t motionIndex = static_cast<size_t>(fullY) * current.width + fullX;
        const int halfDx = motionX[motionIndex] / 2;
        const int halfDy = motionY[motionIndex] / 2;
        const int ax = std::clamp(x + (chroma ? halfDx / 2 : halfDx), 0,
                                  planeWidth - 1);
        const int ay = std::clamp(y + (chroma ? halfDy / 2 : halfDy), 0,
                                  planeHeight - 1);
        const int bx = std::clamp(x - (chroma ? halfDx / 2 : halfDx), 0,
                                  planeWidth - 1);
        const int by = std::clamp(y - (chroma ? halfDy / 2 : halfDy), 0,
                                  planeHeight - 1);
        const unsigned av = a[static_cast<size_t>(ay) * current.linesize[plane] + ax];
        const unsigned bv = b[static_cast<size_t>(by) * next.linesize[plane] + bx];
        dst[static_cast<size_t>(y) * current.linesize[plane] + x] =
            static_cast<uint8_t>((av + bv + 1u) / 2u);
      }
    }
  }
  return true;
}

float codecMotionConfidence(const std::vector<MvEntry> &mvs, int width,
                            int height) {
  float emptyMotionConfidence = 0.5f;
  if (const char *value =
          std::getenv("TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE")) {
    emptyMotionConfidence =
        std::clamp(std::strtof(value, nullptr), 0.0f, 1.0f);
  }
  if (width <= 0 || height <= 0 || mvs.empty())
    return mvs.empty() ? emptyMotionConfidence : 0.0f;
  // Keep the baseline arm reproducible: it must not inherit the newly
  // combined local-confidence factor merely because the same binary is used.
  // Explicit confidence-map and integrated-history arms opt into the new
  // combination even when another diagnostic disables the broad best-findings
  // profile.
  const bool includeLocalConfidence =
      std::getenv("TFORGE_FSR4_DISABLE_BEST_FINDINGS") == nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_MOTION_CONFIDENCE_MAP") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE") != nullptr;
  return MotionEstimator::aggregateConfidence(
      mvs, width, height, emptyMotionConfidence, includeLocalConfidence);
}

float motionLimitMultiplier() {
  // The historical limit was 16 block-diagonals. Keep that exact default for
  // normal playback; quality experiments can lower it to reject codec vectors
  // that are numerically valid but implausibly far from their destination.
  static const float value = [] {
    const char *env =
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS");
    if (!env || !*env)
      return 16.0f;
    char *end = nullptr;
    const float parsed = std::strtof(env, &end);
    if (end == env || *end != '\0' || !std::isfinite(parsed))
      return 16.0f;
    return std::clamp(parsed, 0.25f, 16.0f);
  }();
  return value;
}

// The legacy TFORGE_FSR4_EXPERIMENTAL_PREVIOUS_REFERENCE_ONLY setting and
// the former mv.source <= 0 predicate are retained here as historical
// contract names only. Immediate-previous filtering is now unconditional.
std::vector<MvEntry> pastReferenceMotion(const std::vector<MvEntry> &mvs,
                                         bool rejectBFrameMotion,
                                         bool isBFrame) {
  // FFmpeg's source field only identifies reference direction. A non-positive
  // value does not prove that the vector targets the immediately previous
  // decoded frame: source=0 is ambiguous and values such as source=-2 may
  // target an older past reference. Fail closed because this player owns only
  // the immediately previous history image.
  std::vector<MvEntry> past;
  past.reserve(mvs.size());
  for (const MvEntry &mv : mvs) {
    if (rejectBFrameMotion && isBFrame)
      continue;
    // Positive source indices point at future reference pictures. They are
    // useful to the codec, but are not valid history reprojection vectors for
    // this causal player unless a future frame has independently been
    // timestamp- and motion-validated (which this path does not do).
    // Reject malformed codec side data before it can influence either the
    // confidence estimate or the temporal reprojection texture. In
    // particular, absurd vectors are usually a corrupt/missing reference,
    // not useful motion information.
    const int blockW = std::max(1, static_cast<int>(mv.w));
    const int blockH = std::max(1, static_cast<int>(mv.h));
    const float maxDisplacement =
        4.0f * std::hypot(static_cast<float>(blockW),
                          static_cast<float>(blockH));
    // Keep only FFmpeg's immediately previous-reference marker. Future,
    // ambiguous, and older-reference vectors must never be silently applied
    // to the previous-frame history texture.
    if (mv.source == -1 &&
        std::isfinite(mv.mvX) && std::isfinite(mv.mvY) &&
        std::hypot(mv.mvX, mv.mvY) <=
            maxDisplacement * motionLimitMultiplier())
      past.push_back(mv);
  }
  return past;
}

bool dumpCausalMotionFrame(const std::filesystem::path &path,
                           const DecodedVideoFrame &frame, bool reset,
                           float histogramDelta, float avgLumaDelta,
                           float motionConfidence,
                           const std::vector<MvEntry> &causalMotion,
                           uint32_t targetW, uint32_t targetH,
                           uint32_t frameIndex) {
  // This is a diagnostic artifact only. It records the sparse source-space
  // vectors after the same causal filtering used by the FSR path, before the
  // existing model-coordinate scaling/upload. The Python assembler later
  // validates and expands the records for metric extraction.
  std::error_code directoryError;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), directoryError);
  if (directoryError) {
    logWarn("PlaybackEngine: cannot create motion sidecar directory {}: {}",
            path.parent_path().string(), directoryError.message());
    return false;
  }

  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    logWarn("PlaybackEngine: cannot write motion sidecar frame {}", path.string());
    return false;
  }
  // Frame zero has no previous reference in a causal sequence.  Preserve the
  // transition explicitly instead of claiming its vectors are usable history
  // motion; later frames retain the filtered vectors for diagnostics.
  const std::vector<MvEntry> frameMotion =
      frameIndex == 0 ? std::vector<MvEntry>{} : causalMotion;
  output << std::setprecision(9);
  output << "{\n"
         << "  \"frameIndex\": " << frameIndex << ",\n"
         << "  \"ptsUs\": " << frame.ptsUs << ",\n"
         << "  \"reset\": " << (reset ? "true" : "false") << ",\n"
         << "  \"histogramDelta\": " << histogramDelta << ",\n"
         << "  \"avgLumaDelta\": " << avgLumaDelta << ",\n"
         << "  \"motionConfidence\": " << motionConfidence << ",\n"
         << "  \"motionAvailable\": "
         << (!frameMotion.empty() ? "true" : "false") << ",\n"
         << "  \"vectors\": [";
  for (size_t index = 0; index < frameMotion.size(); ++index) {
    const MvEntry &motion = frameMotion[index];
    if (index != 0) output << ',';
    output << "\n    {\"dstX\": " << static_cast<int>(motion.dstX)
           << ", \"dstY\": " << static_cast<int>(motion.dstY)
           << ", \"mvX\": " << motion.mvX
           << ", \"mvY\": " << motion.mvY
           << ", \"w\": " << static_cast<int>(motion.w)
           << ", \"h\": " << static_cast<int>(motion.h)
           << ", \"source\": " << static_cast<int>(motion.source)
           << ", \"confidence\": "
           << std::clamp(motion.confidence, 0.0f, 1.0f) << '}';
  }
  if (!frameMotion.empty()) output << '\n';
  output << "  ],\n"
         << "  \"sourceWidth\": " << frame.width << ",\n"
         << "  \"sourceHeight\": " << frame.height << ",\n"
         << "  \"targetWidth\": " << targetW << ",\n"
         << "  \"targetHeight\": " << targetH << "\n"
         << "}\n";
  if (!output.good()) {
    logWarn("PlaybackEngine: motion sidecar frame write failed: {}", path.string());
    return false;
  }
  return true;
}

bool dumpEventTraceFrame(const std::filesystem::path &path,
                         const DecodedVideoFrame &frame,
                         uint32_t eventIndex,
                         bool forcedReset,
                         const SideBufferInputs &sideInputs,
                         float ptsDeltaMs) {
  // Authoritative runtime evidence for an event-spanning capture. This records
  // the detector decision and its inputs, not a conclusion derived from image
  // error. The capture assembler adds candidate/scene/config identity and the
  // explicit metric thresholds after the player exits successfully.
  std::error_code directoryError;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), directoryError);
  if (directoryError) {
    logWarn("PlaybackEngine: cannot create event trace directory {}: {}",
            path.parent_path().string(), directoryError.message());
    return false;
  }

  const bool detectorSceneCut = sideInputs.reset && !forcedReset;
  const bool event = forcedReset || detectorSceneCut;
  const char *cause = forcedReset && detectorSceneCut
                          ? "forced_reset_and_detector_scene_cut"
                      : forcedReset ? "forced_reset"
                      : detectorSceneCut ? "detector_scene_cut"
                                         : "none";
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    logWarn("PlaybackEngine: cannot write event trace frame {}", path.string());
    return false;
  }
  output << std::setprecision(9);
  output << "{\n"
         << "  \"schema\": \"temporal_forge.event_trace.v1\",\n"
         << "  \"eventIndex\": " << eventIndex << ",\n"
         << "  \"eventFrameIndex\": " << eventIndex << ",\n"
         << "  \"decoderReceiveIndex\": " << frame.frameIndex << ",\n"
         << "  \"transitionIndex\": "
         << (eventIndex == 0 ? "null" : std::to_string(eventIndex - 1))
         << ",\n"
         << "  \"ptsUs\": " << frame.ptsUs << ",\n"
         << "  \"ptsDeltaMs\": " << ptsDeltaMs << ",\n"
         // Record the source/render-space jitter actually associated with
         // this event-trace frame. Upstream: SideBufferSynth's selected
         // phase; downstream: capture review and timing audits. This is
         // diagnostic provenance only and does not alter FSR inputs.
         << "  \"jitterX\": " << sideInputs.jitterX << ",\n"
         << "  \"jitterY\": " << sideInputs.jitterY << ",\n"
         << "  \"jitterApplied\": "
         << ((sideInputs.jitterX != 0.0f || sideInputs.jitterY != 0.0f)
                 ? "true"
                 : "false")
         << ",\n"
         << "  \"reset\": " << (sideInputs.reset ? "true" : "false")
         << ",\n"
         << "  \"forcedReset\": " << (forcedReset ? "true" : "false")
         << ",\n"
         << "  \"detectorSceneCut\": "
         << (detectorSceneCut ? "true" : "false") << ",\n"
         << "  \"resetCause\": \"" << cause << "\",\n"
         << "  \"ghostCause\": \"" << cause << "\",\n"
         << "  \"detectorInputs\": {\n"
         << "    \"histogramDelta\": " << sideInputs.histogramDelta << ",\n"
         << "    \"avgLumaDelta\": " << sideInputs.avgLumaDelta << ",\n"
         << "    \"motionConfidence\": " << sideInputs.motionConfidence << ",\n"
         << "    \"reactiveAverage\": " << sideInputs.reactiveAverage << ",\n"
         << "    \"ptsGapMs\": " << ptsDeltaMs << ",\n"
         << "    \"expectedFrameIntervalMs\": "
         << sideInputs.expectedFrameIntervalMs << "\n"
         << "  },\n"
         << "  \"thresholdProvenance\": {\n"
         << "    \"contract\": \"side_buffer_scene_cut.v1\",\n"
         << "    \"implementation\": \"SideBufferSynth::shouldReset\",\n"
         << "    \"histogramDeltaGreaterThan\": 0.65,\n"
         << "    \"motionConfidenceLessThan\": 0.15,\n"
         << "    \"ptsGapMultiplierGreaterThan\": 2.5\n"
         << "  },\n"
         << "  \"event\": " << (event ? "true" : "false") << "\n"
         << "}\n";
  if (!output.good()) {
    logWarn("PlaybackEngine: event trace frame write failed: {}", path.string());
    return false;
  }
  return true;
}

// Map sparse block coverage into the model grid. The codec/dense estimators
// produce current-destination-to-previous-reference vectors in source pixels;
// the dense motion image stores model-pixel magnitudes, and the prepass then
// applies the model-to-output scale. Coverage positions and vector magnitudes
// therefore need the same source-to-model conversion exactly once.
std::vector<MvEntry> scaleMotionCoverageToModel(
    const std::vector<MvEntry> &mvs, int sourceW, int sourceH,
    uint32_t modelW, uint32_t modelH) {
  if (sourceW <= 0 || sourceH <= 0 || modelW == 0 || modelH == 0)
    return {};
  const float sx = static_cast<float>(modelW) / sourceW;
  const float sy = static_cast<float>(modelH) / sourceH;
  std::vector<MvEntry> scaled;
  scaled.reserve(mvs.size());
  for (MvEntry mv : mvs) {
    mv.dstX = static_cast<int16_t>(std::clamp(
        std::lround(static_cast<float>(mv.dstX) * sx), -32768l, 32767l));
    mv.dstY = static_cast<int16_t>(std::clamp(
        std::lround(static_cast<float>(mv.dstY) * sy), -32768l, 32767l));
    mv.w = static_cast<uint8_t>(std::clamp(
        std::lround(static_cast<float>(std::max(1, static_cast<int>(mv.w))) * sx),
        1l, 255l));
    mv.h = static_cast<uint8_t>(std::clamp(
        std::lround(static_cast<float>(std::max(1, static_cast<int>(mv.h))) * sy),
        1l, 255l));
    mv.mvX *= sx;
    mv.mvY *= sy;
    scaled.push_back(mv);
  }
  return scaled;
}

// Order overlapping sparse vectors so the existing last-writer coverage rule
// selects the most trusted vector instead of whichever block happened to be
// uploaded last. Upstream: codec, fallback, or replay motion plus optional
// confidence scoring. Downstream: both CPU and GPU expanders, which preserve
// this order while stamping per-pixel motion. This is opt-in because the
// established baseline must remain byte-for-byte behaviorally unchanged.
void orderMotionByConfidence(std::vector<MvEntry> &mvs) {
  std::stable_sort(mvs.begin(), mvs.end(), [](const MvEntry &a,
                                               const MvEntry &b) {
    const float aConfidence = std::isfinite(a.confidence)
        ? std::clamp(a.confidence, 0.0f, 1.0f) : 0.0f;
    const float bConfidence = std::isfinite(b.confidence)
        ? std::clamp(b.confidence, 0.0f, 1.0f) : 0.0f;
    return aConfidence < bConfidence;
  });
}

bool loadDenseMotionReplay(const char *path, uint64_t frameIndex,
                           uint64_t denseMotionReplaySeekGeneration,
                           int sourceW, int sourceH,
                           std::vector<MvEntry> &out) {
  // This cache is decode-thread-owned. The replay sidecar is loaded once per
  // path, then indexed by absolute decoder frame number so warmup frames and
  // relative image-dump numbering cannot shift correspondence silently.
  struct Cache {
    std::string path;
    std::unordered_map<uint64_t, std::vector<MvEntry>> frames;
    bool loaded = false;
    bool warned = false;
    // Replay sidecars use capture-relative frame IDs, while the decoder's
    // frameIndex may begin after benchmark warmup. Establish the offset from
    // the first frame that asks for replay so warmup cannot cause a silent
    // fail-closed fallback to the baseline.
    uint64_t frameBase = 0;
    bool frameBaseSet = false;
    uint64_t seekGeneration = 0;
    bool seekGenerationSet = false;
    // The sidecar dimensions are part of the motion contract. They must
    // match the decoded source dimensions on every lookup, because a replay
    // file from another input resolution would otherwise apply plausible but
    // spatially wrong vectors to the current frame.
    int sidecarSourceW = 0;
    int sidecarSourceH = 0;
  };
  static Cache cache;
  if (!path || !*path)
    return false;
  if (!cache.loaded || cache.path != path) {
    cache = Cache{};
    cache.path = path;
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) {
      logWarn("PlaybackEngine: dense motion replay cannot open {}", path);
      cache.warned = true;
      return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      logWarn("PlaybackEngine: dense motion replay JSON is invalid: {}", path);
      cache.warned = true;
      return false;
    }
    const QJsonObject root = document.object();
    const int sidecarW = root.value(QStringLiteral("sourceWidth")).toInt();
    const int sidecarH = root.value(QStringLiteral("sourceHeight")).toInt();
    if (root.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("temporal_forge.codec_motion.v1") ||
        root.value(QStringLiteral("coordinateDomain")).toString() !=
            QStringLiteral("current_destination_to_previous_reference") ||
        root.value(QStringLiteral("motionUnits")).toString() !=
            QStringLiteral("source_pixels") ||
        root.value(QStringLiteral("sampleConvention")).toString() !=
            QStringLiteral("destination_plus_motion") ||
        root.value(QStringLiteral("frameIndexBase")).toString() !=
            QStringLiteral("capture_relative") ||
        sidecarW <= 0 || sidecarH <= 0) {
      logWarn("PlaybackEngine: dense motion replay schema is unsupported: {}",
              path);
      cache.warned = true;
      return false;
    }
    const QJsonArray frames = root.value(QStringLiteral("frames")).toArray();
    for (const QJsonValue &frameValue : frames) {
      const QJsonObject frame = frameValue.toObject();
      if (!frameValue.isObject() || !frame.contains(QStringLiteral("frameIndex")) ||
          !frame.value(QStringLiteral("frameIndex")).isDouble())
        continue;
      const int64_t rawIndex =
          static_cast<int64_t>(frame.value(QStringLiteral("frameIndex")).toDouble(-1));
      if (rawIndex < 0)
        continue;
      std::vector<MvEntry> vectors;
      const QJsonArray values = frame.value(QStringLiteral("vectors")).toArray();
      for (const QJsonValue &value : values) {
        const QJsonObject item = value.toObject();
        const double x = item.value(QStringLiteral("dstX")).toDouble(qQNaN());
        const double y = item.value(QStringLiteral("dstY")).toDouble(qQNaN());
        const double mvX = item.value(QStringLiteral("mvX")).toDouble(qQNaN());
        const double mvY = item.value(QStringLiteral("mvY")).toDouble(qQNaN());
        const double width = item.value(QStringLiteral("w")).toDouble(qQNaN());
        const double height = item.value(QStringLiteral("h")).toDouble(qQNaN());
        const double confidence =
            item.value(QStringLiteral("confidence")).toDouble(1.0);
        const int source = item.value(QStringLiteral("source")).toInt(1);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(mvX) ||
            !std::isfinite(mvY) || !std::isfinite(width) ||
            !std::isfinite(height) || std::floor(x) != x || std::floor(y) != y ||
            std::floor(width) != width || std::floor(height) != height ||
            !std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0 ||
            width <= 0.0 || height <= 0.0 || source != -1 ||
            std::abs(mvX) > static_cast<double>(sourceW) ||
            std::abs(mvY) > static_cast<double>(sourceH) ||
            x < -32768.0 || x > 32767.0 || y < -32768.0 || y > 32767.0 ||
            width > 255.0 || height > 255.0)
          continue;
        MvEntry motion;
        motion.dstX = static_cast<int16_t>(x);
        motion.dstY = static_cast<int16_t>(y);
        motion.mvX = static_cast<float>(mvX);
        motion.mvY = static_cast<float>(mvY);
        motion.w = static_cast<uint8_t>(width);
        motion.h = static_cast<uint8_t>(height);
        motion.source = static_cast<int8_t>(std::clamp(source, -128, 0));
        motion.confidence = static_cast<float>(confidence);
        vectors.push_back(motion);
      }
      cache.frames[static_cast<uint64_t>(rawIndex)] = std::move(vectors);
    }
    cache.sidecarSourceW = sidecarW;
    cache.sidecarSourceH = sidecarH;
    cache.loaded = true;
  }
  if (sourceW <= 0 || sourceH <= 0 || cache.sidecarSourceW != sourceW ||
      cache.sidecarSourceH != sourceH) {
    if (!cache.warned) {
      logWarn("PlaybackEngine: dense motion replay dimensions {}x{} do not "
              "match decoded source {}x{}: {}",
              cache.sidecarSourceW, cache.sidecarSourceH, sourceW, sourceH,
              path);
      cache.warned = true;
    }
    return false;
  }
  // Decoder frame indices restart after a seek. Rebase the capture-relative
  // origin for the new seek generation so frame zero is not looked up against
  // the previous sequence's cached frameBase.
  if (!cache.seekGenerationSet ||
      cache.seekGeneration != denseMotionReplaySeekGeneration) {
    cache.seekGeneration = denseMotionReplaySeekGeneration;
    cache.seekGenerationSet = true;
    cache.frameBase = 0;
    cache.frameBaseSet = false;
  }
  if (!cache.frameBaseSet) {
    cache.frameBase = frameIndex;
    cache.frameBaseSet = true;
  }
  if (frameIndex < cache.frameBase)
    return false;
  const auto found = cache.frames.find(frameIndex - cache.frameBase);
  if (found == cache.frames.end())
    return false;
  out = found->second;
  return true;
}

float lookaheadConfidence(const DecodedVideoFrame &current,
                          const DecodedVideoFrame &next) {
  if (current.width <= 0 || current.height <= 0 || next.width != current.width ||
      next.height != current.height || next.ptsUs <= current.ptsUs)
    return 0.0f;
  const int64_t deltaUs = next.ptsUs - current.ptsUs;
  const int64_t expectedUs = current.durationUs > 0 ? current.durationUs : 16667;
  if (deltaUs > std::max<int64_t>(250000, expectedUs * 8))
    return 0.0f;
  const LumaBuffer a = makeAnalysisLuma(current);
  const LumaBuffer b = makeAnalysisLuma(next);
  if (a.width == 0 || a.width != b.width || a.height != b.height)
    return 0.0f;
  double mad = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i)
    mad += std::abs(static_cast<double>(a.data[i]) - b.data[i]);
  mad /= static_cast<double>(a.data.size());
  // This score is analysis-only: a large change lowers history trust, while
  // a stable next frame confirms that the current frame is not an isolated
  // decode artifact. It never causes the next frame's pixels to be blended.
  return static_cast<float>(std::clamp(1.0 - mad * 4.0, 0.05, 1.0));
}

} // namespace

PlaybackEngine::PlaybackEngine(QObject *parent) : QObject(parent) {
  // Position/UI refresh ticker (Qt thread). Spec 05 wants current timestamp
  // in the UI; this just polls the clock ~10x/sec.
  pollTimer_.setInterval(100);
  connect(&pollTimer_, &QTimer::timeout, this, &PlaybackEngine::onPollTick);
  pollTimer_.start();
}

PlaybackEngine::~PlaybackEngine() {
  close();
  stopThreads();
}

void PlaybackEngine::promoteStableFsrViewport() {
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const int64_t changedUs = fsrViewportChangedUs_.load(std::memory_order_acquire);
  if (changedUs == 0 || nowUs - changedUs < 200000)
    return;
  const uint32_t requestedW = fsrViewportW_.load(std::memory_order_acquire);
  const uint32_t requestedH = fsrViewportH_.load(std::memory_order_acquire);
  const uint32_t targetW = fsrTargetViewportW_.load(std::memory_order_acquire);
  const uint32_t targetH = fsrTargetViewportH_.load(std::memory_order_acquire);
  if (requestedW == targetW && requestedH == targetH)
    return;
  fsrTargetViewportW_.store(requestedW, std::memory_order_release);
  fsrTargetViewportH_.store(requestedH, std::memory_order_release);
}

void PlaybackEngine::setVulkanHandles(VkPhysicalDevice physical,
                                      VkDevice device, VkQueue queue,
                                      uint32_t queueFamily,
                                      uint32_t presentationQueueFamily,
                                      VkInstance instance) {
  vkPhysical_ = physical;
  vkDevice_ = device;
  vkQueue_ = queue;
  vkQueueFamily_ = queueFamily;
  vkPresentationQueueFamily_ = presentationQueueFamily;
  if (vkDevice_ == VK_NULL_HANDLE) {
    logInfo("PlaybackEngine: no Vulkan device — FSR4 upscaling disabled");
    return;
  }
  // Probe the exact physical device selected by VulkanContext. The instance
  // is required to resolve cooperative-matrix properties, including the
  // FP16 fallback flag used by diagnostic generic dispatch. The production
  // RDNA3 profile remains INT8 DOT4; this only stops startup from discarding
  // capability facts discovered by the standalone probe.
  vkCap_ = GpuCapabilityProbe::probe(physical, instance);
  if (!vkCap_.valid) {
    logWarn("PlaybackEngine: selected Vulkan device is not FSR4-capable: {}",
            vkCap_.failReason);
  }
  // When Vulkan is available, default to EASU-only mode so frames are always
  // GPU-upscaled (2x edge-adaptive) even when the neural FSR4 path is off.
  // setFsr4Enabled(true) will clear this when the user enables FSR4.
  if (!fsr4Enabled_.load(std::memory_order_acquire))
    easuOnlyMode_.store(true, std::memory_order_release);
  logInfo("PlaybackEngine: Vulkan handles set; FSR4 will be initialized on "
          "first frame");
}

void PlaybackEngine::setFsr4Enabled(bool enabled) {
  if (fsr4Enabled_.load(std::memory_order_acquire) == enabled)
    return;
  fsr4Enabled_.store(enabled, std::memory_order_release);
  fsr4FrameReady_.store(false, std::memory_order_release);
  fsrTemporalResetRequested_.store(true, std::memory_order_release);
  fsr4NativePassthrough_.store(false, std::memory_order_release);
  if (!enabled) {
    // Disabling FSR4 neural upscaling. Tear down the harness (weights, CNN
    // pipelines) but keep the uploader if Vulkan is present — it will run
    // EASU-only mode (GPU 2x edge-adaptive upscale) so the off-path still
    // looks good instead of falling back to CPU/bilinear pixelation.
    if (vkDevice_ != VK_NULL_HANDLE) {
      easuOnlyMode_.store(true, std::memory_order_release);
      logInfo("PlaybackEngine: FSR4 off — entering EASU-only GPU upscale mode");
    } else {
      teardownFsr4Path();
    }
    fsr4ProofRun_.store(false, std::memory_order_release);
    fsr4ProofPassed_.store(false, std::memory_order_release);
    lastFsr4DispatchMs_.store(0.0, std::memory_order_release);
    lastFsr4GpuMs_.store(0.0, std::memory_order_release);
  } else {
    // Re-enabling FSR4: leave EASU-only mode. The decode loop will re-init
    // the full FSR4 path (harness + weights) on the next frame.
    easuOnlyMode_.store(false, std::memory_order_release);
  }
  logInfo("PlaybackEngine: FSR4 upscaling {}",
          enabled ? "enabled" : "disabled");
  emit fsr4StatusChanged();
}

void PlaybackEngine::teardownFsr4Path() {
  // Ask the decode loop to stop dispatching FSR4 frames. The loop checks
  // this flag between input uploads and bails out without touching the
  // GPU fence, so we cannot deadlock against a half-finished dispatch.
  fsrAbortRequested_.store(true, std::memory_order_release);
  // Wake any sleeping decode thread so it observes the flag promptly.
  pktCv_.notify_all();
  frameCv_.notify_one();

  // Mark not-ready first so VideoSurfaceItem stops exposing the images
  // immediately, even before we finish tearing them down.
  fsr4Ready_.store(false, std::memory_order_release);
  fsr4FrameReady_.store(false, std::memory_order_release);
  fsrTemporalResetRequested_.store(true, std::memory_order_release);
  fsr4NativePassthrough_.store(false, std::memory_order_release);
  // NOTE: do NOT clear easuOnlyMode_ here — it's a display policy, not a
  // teardown state. The decode loop re-creates the uploader lazily when
  // easuOnlyMode_ stays true (e.g. between file switches with FSR4 off).

  // Hold the dispatch mutex so a decode-thread dispatch that already
  // started recording commands cannot keep using these Vulkan resources
  // while we free them. Any dispatch past this point either completes its
  // queue submit (which the wait-idle below retires) or has not started.
  std::lock_guard<std::mutex> dispatchLock(fsrDispatchMutex_);

  // The decode thread may still be inside a dispatch that writes to the
  // Vulkan queue. Wait for the queue to drain so any in-flight command
  // buffer completes before we destroy the resources it references. The
  // Qt render thread uses the same logical device, so its reads against
  // these images are also guaranteed to be retired by this wait.
  if (vkDevice_ != VK_NULL_HANDLE && vkQueue_ != VK_NULL_HANDLE) {
    vkQueueWaitIdle(vkQueue_);
  }

  // Now safe to free: the decode loop has either finished its current
  // dispatch or aborted before recording any commands referencing the
  // uploader/harness, and no Qt-side texture is sampling the output image.
  fsr4PublishedUploader_.store(nullptr, std::memory_order_release);
  fsr4Uploader_.reset();
  fsr4Harness_.reset();
  fsr4InFlightUploader_.reset();
  fsr4InFlightHarness_.reset();
  fsr4LastSubmittedUploader_ = nullptr;
  fsr4NextDispatchSlot_ = 0;
  fsr4IntermediateUploaders_.clear();
  fsr4IntermediateHarnesses_.clear();
  fsr4PassSizes_.clear();
  fsr4OutW_.store(0, std::memory_order_release);
  fsr4OutH_.store(0, std::memory_order_release);
  lastFsr4DispatchMs_.store(0.0, std::memory_order_release);
  lastFsr4GpuMs_.store(0.0, std::memory_order_release);
  fsr4AppliedSharpness_ = -1.0f;
  fsrAbortRequested_.store(false, std::memory_order_release);
  logInfo("PlaybackEngine: FSR4 path torn down");
  emit fsr4StatusChanged();
}

// initFsr4Path: set up the live FSR4 dispatch path for a new source size.
//
// Called by: videoDecodeLoop when fsr4Enabled_ is set and the harness needs
//            (re)creation after a source/preset change. Runs on the decode thread.
// Calls:    WeightBlobLoader::load, Fsr4DispatchHarness::create (pipeline + weight
//          upload), GpuImageUploader::allocate, selects the progressive pass chain
//          via fsrProgressivePassSizes / nativeInt8UltraPerformanceTarget.
// Returns: false (graceful degradation — raw frames) when Vulkan is absent, the
//          weight blob fails to load, or pipeline creation fails. The caller
//          then displays raw decoded frames with no upscaling.
// Notes:   Holds fsrDispatchMutex_ for the duration so teardownFsr4Path cannot
//          free the harness mid-create. Sets fsr4Ready_ on success.
bool PlaybackEngine::initFsr4Path(int decodedW, int decodedH, int modelW,
                                  int modelH) {
  if (vkDevice_ == VK_NULL_HANDLE || !vkCap_.valid)
    return false;
  if (decodedW <= 0 || decodedH <= 0 || modelW <= 0 || modelH <= 0)
    return false;

  // FSR renders to the fitted presentation target. The multiplier determines
  // how far below that fixed target the prefiltered FSR input is generated.
  float scale = fsrScale_.load();
  if (const char *env = std::getenv("TFORGE_FSR4_FORCE_SCALE")) {
    char *end = nullptr;
    const float forced = std::strtof(env, &end);
    if (end != env && std::isfinite(forced) && forced >= 1.0f)
      scale = forced;
  }
  // The neural target is independent of the window.  A forced benchmark
  // viewport remains an explicit diagnostic override, but normal playback
  // uses the fixed native pack target and lets the presentation scaler handle
  // window size changes.  Feeding the window size into this calculation made
  // a small 4:3 source allocate a 1920x1440 generic graph and destroyed the
  // expected multiplier performance curve.
  const bool forcedViewport = std::getenv("TFORGE_FSR4_FORCE_VIEWPORT") != nullptr;
  const Size2D nativeOutputTarget = nativeInt8FixedTarget(
      alignEven(static_cast<uint32_t>(decodedW)),
      alignEven(static_cast<uint32_t>(decodedH)));
  uint32_t outW = nativeOutputTarget.width;
  uint32_t outH = nativeOutputTarget.height;
  if (forcedViewport || outW == 0 || outH == 0) {
    const auto viewport = fsrViewportForBenchmark(
        std::max(2u, fsrTargetViewportW_.load(std::memory_order_acquire)),
        std::max(2u, fsrTargetViewportH_.load(std::memory_order_acquire)));
    // A benchmark viewport such as 1920x1080 is an explicit output contract.
    // Preserve the fixed native INT8 shape when it is exactly that requested
    // target; fitting 426x240 mathematically produces 1918x1080 and silently
    // routes the frame through the much slower generic graph.
    if (!(forcedViewport && nativeOutputTarget.width == viewport.first &&
          nativeOutputTarget.height == viewport.second)) {
      const double fit = std::min(
          static_cast<double>(viewport.first) / decodedW,
          static_cast<double>(viewport.second) / decodedH);
      outW = std::max(
          2u, alignEven(static_cast<uint32_t>(std::round(decodedW * fit))));
      outH = std::max(
          2u, alignEven(static_cast<uint32_t>(std::round(decodedH * fit))));
    }
  }

  // Generic RE passes must use the blob matching the selected preset. The
  // native fixed-shape packs carry their own initializer and bypass this.
  const Fsr4Preset blobPreset =
      scale <= 1.01f   ? Fsr4Preset::Native
      : scale < 1.60f  ? Fsr4Preset::Quality
      : scale < 1.90f  ? Fsr4Preset::Balanced
                       : scale < 2.99f ? Fsr4Preset::Performance
                                       : Fsr4Preset::UltraPerf;

  // v4.1 contains one shared initializer for all standard multiplier
  // presets. The DRS initializer is a separate retrained network and must not
  // be substituted for the normal Quality/Balanced/Performance path.
  // DRS is an optional retrained initializer for window-adaptive experiments;
  // standard multiplier names continue to resolve to the shared standard
  // blob unless this explicit runtime policy is enabled.
  const bool useDrs = std::getenv("TFORGE_FSR4_DRS") != nullptr;
  const Fsr4Preset blobFilePreset = useDrs ? Fsr4Preset::Drs : Fsr4Preset::Quality;

  // Load the packed RE weight blob only when a generic fallback pass needs it.
  // Fixed-shape native INT8 packs carry their own initializer and do not use
  // the legacy 131072-byte blob at all.
  auto ensureWeightBlob = [&]() -> bool {
    if (!fsr4BlobStorage_.empty() && fsr4LoadedBlobPreset_ == blobFilePreset)
      return true;
    fsr4BlobStorage_.clear();
    fsr4Blob_ = {};
    const std::string blobName =
        WeightBlobLoader::presetFileName(blobFilePreset);
    const char *reRoot = std::getenv("TFORGE_FSR4_RE_ROOT");
    std::filesystem::path blobFile;
    std::vector<std::filesystem::path> candidates;
    if (reRoot && *reRoot) {
      candidates.emplace_back(
          std::filesystem::path(reRoot) /
          ("extracted/v410_initializers/" + blobName));
    }
    for (auto p : {
             std::filesystem::path(
                 "/home/rolaandjayz/ZCodeProject/RE-of-FSR-4.1.0-Upscaling-1.0/"
                 "extracted/v410_initializers/") / blobName,
             std::filesystem::path(
                 "/mnt/workdrive/fsr-re/extracted/v410_initializers/") / blobName,
             std::filesystem::path(
                 "/mnt/workdrive/fsr-re/dist/fsr4-swap/extracted/"
                 "v410_initializers/") / blobName,
             std::filesystem::path("RE-of-FSR-4.1.0-Upscaling-1.0/extracted/"
                                   "v410_initializers/") / blobName,
             std::filesystem::path("../RE-of-FSR-4.1.0-Upscaling-1.0/extracted/"
                                   "v410_initializers/") / blobName})
      candidates.push_back(std::move(p));
    for (const auto &p : candidates) {
      if (std::filesystem::exists(p)) { blobFile = p; break; }
    }
    if (blobFile.empty()) {
      logWarn("PlaybackEngine: FSR4 weight blob not found; upscaling disabled");
      return false;
    }
    auto loaded = WeightBlobLoader::load(blobFilePreset, blobFile.string());
    if (!loaded.ok) {
      logWarn("PlaybackEngine: FSR4 weight blob load failed ({}); upscaling disabled",
              loaded.failReason);
      return false;
    }
    fsr4Blob_ = WeightBlobLoader::view(loaded);
    fsr4BlobStorage_ = std::move(loaded.data);
    fsr4LoadedBlobPreset_ = blobFilePreset;
    logInfo("PlaybackEngine: FSR4 multiplier {} uses {} blob {} ({}, {} bytes)",
            WeightBlobLoader::presetName(blobPreset),
            useDrs ? "DRS" : "standard",
            WeightBlobLoader::presetName(blobFilePreset), blobName,
            fsr4BlobStorage_.size());
    return true;
  };

  const Size2D sourceSize{alignEven(static_cast<uint32_t>(decodedW)),
                          alignEven(static_cast<uint32_t>(decodedH))};
  const Size2D targetSize{outW, outH};
  const Size2D nativeTarget =
      nativeInt8FixedTarget(sourceSize.width, sourceSize.height);
  const bool nativeFixedTarget =
      nativeTarget.width != 0 && targetSize.width == nativeTarget.width &&
      targetSize.height == nativeTarget.height;
  std::vector<Size2D> requested;
  if (const char *env = std::getenv("TFORGE_FSR4_CHAIN_PASSES")) {
    char *end = nullptr;
    const long count = std::strtol(env, &end, 10);
    if (end != env && count > 0) {
      // An explicit chain count is an experiment contract: build exactly that
      // many geometrically progressive passes, keeping the expensive neural
      // work at smaller sizes until the final pass. The old override repeated
      // the final target for every pass and made the experiment needlessly
      // expensive.
      requested.reserve(static_cast<size_t>(count));
      for (long pass = 1; pass <= count; ++pass) {
        const double fraction = static_cast<double>(pass) /
                                static_cast<double>(count);
        const auto progressiveSize = [](uint32_t source, uint32_t target,
                                        double fraction) {
          if (source >= target)
            return target;
          const double ratio = static_cast<double>(target) /
                               static_cast<double>(source);
          const auto value = static_cast<uint32_t>(std::ceil(
              static_cast<double>(source) * std::pow(ratio, fraction)));
          return std::min(target, alignEven(value));
        };
        requested.push_back({
            progressiveSize(sourceSize.width, targetSize.width, fraction),
            progressiveSize(sourceSize.height, targetSize.height, fraction)});
      }
    }
  } else {
    // Normal FSR is a single reconstruction from the multiplier-derived
    // input to the fixed presentation target. Progressive chaining was the
    // source of the inverted performance curve: larger multipliers created
    // more intermediate output images instead of reducing input work.
    requested.clear();
  }
  // Supported fixed-shape INT8 tiers are already the optimized solution for
  // low-resolution video. Do not replace a sub-millisecond native graph with
  // a generic progressive chain unless the chain is explicitly requested.
  if (nativeFixedTarget && !std::getenv("TFORGE_FSR4_CHAIN_PASSES"))
    requested.clear();
  if (requested.empty() || requested.back().width != targetSize.width ||
      requested.back().height != targetSize.height)
    requested.push_back(targetSize);

  const bool resourcesPresent =
      fsr4Harness_ && fsr4Uploader_ &&
      fsr4IntermediateUploaders_.size() + 1 == requested.size() &&
      fsr4IntermediateHarnesses_.size() + 1 == requested.size() &&
      (requested.size() != 1 ||
       (fsr4InFlightHarness_ && fsr4InFlightUploader_));
  const bool dimensionsMatch = resourcesPresent &&
      fsr4PassSizes_.size() == requested.size() &&
      std::equal(fsr4PassSizes_.begin(), fsr4PassSizes_.end(), requested.begin(),
                 [](const Size2D &a, const Size2D &b) {
                   return a.width == b.width && a.height == b.height;
                 });
  if (!dimensionsMatch) {
    if (vkQueue_ != VK_NULL_HANDLE)
      vkQueueWaitIdle(vkQueue_);
    fsr4Uploader_.reset();
    fsr4Harness_.reset();
    fsr4PublishedUploader_.store(nullptr, std::memory_order_release);
    fsr4InFlightUploader_.reset();
    fsr4InFlightHarness_.reset();
    fsr4LastSubmittedUploader_ = nullptr;
    fsr4NextDispatchSlot_ = 0;
    fsr4IntermediateUploaders_.clear();
    fsr4IntermediateHarnesses_.clear();
    fsr4PassSizes_ = requested;
  }

  auto createPass = [&](uint32_t decodedPassW, uint32_t decodedPassH,
                        uint32_t passSourceW, uint32_t passSourceH,
                        const Size2D &passTarget,
                        std::unique_ptr<Fsr4DispatchHarness> &h,
                        std::unique_ptr<GpuImageUploader> &u) -> bool {
    logInfo("PlaybackEngine: FSR4 pass decoded {}x{} -> model {}x{} -> {}x{}",
            decodedPassW, decodedPassH, passSourceW, passSourceH,
            passTarget.width, passTarget.height);
    h = std::make_unique<Fsr4DispatchHarness>();
    // The measured base-only composition wins on severe upscales, but the
    // legacy composition is stronger at the normal 1.5x 720p-to-1080p tier.
    // Apply that scale-aware policy only to the checked-in default. An
    // explicit TFORGE_QUALITY_LAB_CONFIG remains a deliberate experiment and
    // must retain its requested composition at every scale.
    QualityLabConfig passQualityLabConfig = qualityLabConfig_;
    const bool defaultScaleAwareQualityLab =
        qualityLabConfig_.enabled &&
        std::getenv("TFORGE_QUALITY_LAB_CONFIG") == nullptr;
    const float passScale = passSourceW == 0
                                ? scale
                                : static_cast<float>(passTarget.width) /
                                      static_cast<float>(passSourceW);
    if (defaultScaleAwareQualityLab && passScale < 3.0f)
      passQualityLabConfig.enabled = false;
    h->setQualityLabConfig(passQualityLabConfig);
    if (!h->init(vkPhysical_, vkDevice_, vkQueue_, vkQueueFamily_, vkCap_))
      return false;
    Fsr4DispatchResources r{};
    r.sourceWidth = passSourceW;
    r.sourceHeight = passSourceH;
    r.outputWidth = passTarget.width;
    r.outputHeight = passTarget.height;
    r.requestedScale = scale;
    if (!h->allocateResources(r))
      return false;
    if (!h->usesNativeInt8() &&
        (!ensureWeightBlob() || !h->uploadWeights(fsr4Blob_)))
      return false;
    u = std::make_unique<GpuImageUploader>();
    if (!u->init(vkPhysical_, vkDevice_, vkQueue_, vkQueueFamily_,
                 vkPresentationQueueFamily_) ||
        !u->allocate(decodedPassW, decodedPassH, passTarget.width,
                     passTarget.height, passSourceW, passSourceH) ||
        !u->transitionOutputToGeneral())
      return false;
    u->setSharpness(sharpness_.load(std::memory_order_acquire));
    u->setPresentationScaler(
        qualityLabPresentationScaler(
            qualityLabConfig_, presentationScaler_.load(std::memory_order_acquire)));
    u->setCompareEnabled(compareEnabled_.load(std::memory_order_acquire));
    return true;
  };

  if (!dimensionsMatch) {
    // Preserve the decoder's exact first-pass dimensions. Later targets are
    // even-aligned by the progressive planner, but changing the first input
    // dimensions makes the upload path reject valid odd-width video frames.
    uint32_t passSourceW = static_cast<uint32_t>(modelW);
    uint32_t passSourceH = static_cast<uint32_t>(modelH);
    for (size_t i = 0; i < requested.size(); ++i) {
      if (i + 1 == requested.size()) {
        const uint32_t decodedPassW = i == 0 ? static_cast<uint32_t>(decodedW)
                                             : passSourceW;
        const uint32_t decodedPassH = i == 0 ? static_cast<uint32_t>(decodedH)
                                             : passSourceH;
        if (!createPass(decodedPassW, decodedPassH, passSourceW, passSourceH,
                        requested[i], fsr4Harness_, fsr4Uploader_)) {
          fsr4PassSizes_.clear();
          return false;
        }
      } else {
        fsr4IntermediateHarnesses_.push_back(nullptr);
        fsr4IntermediateUploaders_.push_back(nullptr);
        const uint32_t decodedPassW = i == 0 ? static_cast<uint32_t>(decodedW)
                                             : passSourceW;
        const uint32_t decodedPassH = i == 0 ? static_cast<uint32_t>(decodedH)
                                             : passSourceH;
        if (!createPass(decodedPassW, decodedPassH, passSourceW, passSourceH,
                        requested[i],
                        fsr4IntermediateHarnesses_.back(),
                        fsr4IntermediateUploaders_.back())) {
          fsr4PassSizes_.clear();
          return false;
        }
      }
      passSourceW = requested[i].width;
      passSourceH = requested[i].height;
    }
  }

  // Keep a second complete single-pass resource set. Its color upload,
  // output, history, recurrent state, command buffer, and fence are all
  // independent from the published slot, allowing one CPU upload/recording
  // interval to overlap the prior FSR submission. Progressive chains retain
  // the serial path because their intermediate passes have explicit
  // same-frame dependencies.
  if (requested.size() == 1 &&
      std::getenv("TFORGE_FSR4_DISABLE_INFLIGHT") == nullptr &&
      (!fsr4InFlightHarness_ || !fsr4InFlightUploader_)) {
    if (!createPass(static_cast<uint32_t>(decodedW),
                    static_cast<uint32_t>(decodedH), sourceSize.width,
                    sourceSize.height, targetSize, fsr4InFlightHarness_,
                    fsr4InFlightUploader_)) {
      fsr4InFlightHarness_.reset();
      fsr4InFlightUploader_.reset();
      logWarn("PlaybackEngine: second FSR4 in-flight slot unavailable; "
              "using the synchronous slot");
    }
  }
  fsr4AppliedSharpness_ = -1.0f;
  fsr4AppliedCompareEnabled_ = !compareEnabled_.load(std::memory_order_acquire);
  fsr4Uploader_->setSharpness(sharpness_.load(std::memory_order_acquire));
  fsr4Uploader_->setCompareEnabled(
      compareEnabled_.load(std::memory_order_acquire));
  fsr4AppliedSharpness_ = sharpness_.load(std::memory_order_acquire);
  fsr4AppliedCompareEnabled_ = compareEnabled_.load(std::memory_order_acquire);
  // Transition output/history images to GENERAL layout for postpass writes.
  fsr4Uploader_->transitionOutputToGeneral();
  if (fsr4InFlightUploader_)
    fsr4InFlightUploader_->transitionOutputToGeneral();
  fsr4PublishedUploader_.store(fsr4Uploader_.get(), std::memory_order_release);
  fsr4LastSubmittedUploader_ = nullptr;
  fsr4NextDispatchSlot_ = 0;

  fsr4OutW_.store(outW, std::memory_order_release);
  fsr4OutH_.store(outH, std::memory_order_release);
  fsr4FrameReady_.store(false, std::memory_order_release);
  fsr4DumpedOutput_ = false;
  fsr4DumpedModelInput_ = false;
  fsr4DumpedRaw_ = false;
  fsr4SequenceDumpCount_ = 0;
  fsr4SequenceFramesSeen_ = 0;
  fsr4DumpedPresentation_ = false;
  fsr4Ready_.store(true, std::memory_order_release);
  logInfo("PlaybackEngine: FSR4 path ready decoded {}x{} -> model {}x{} -> {}x{}",
          decodedW, decodedH, modelW, modelH, outW, outH);
  logInfo("PlaybackEngine: FSR4 progressive chain passes={}",
          fsr4PassSizes_.size());
  writeRuntimePipelineTrace(
      static_cast<uint32_t>(decodedW), static_cast<uint32_t>(decodedH),
      static_cast<uint32_t>(modelW), static_cast<uint32_t>(modelH), outW, outH,
      scale, fsr4Harness_ && fsr4Harness_->usesNativeInt8(),
      fsr4PassSizes_.size());
  emit fsr4StatusChanged();
  return true;
}

// setFsrViewport: record the current window size + FSR scale (preset ratio).
//
// Called by: FsrController::setWindowSize (every window resize) and setPreset
//            (preset ratio change) — both from the UI thread.
// Calls:    stores fsrViewportW_/H_/fsrScale_; on scale change, clears
//           fsr4Ready_/fsr4FrameReady_ so the decode loop re-initializes the harness.
// Notes:    CRITICAL INVARIANT (regression lesson 2026-07-21): window size only
//           affects presentation, never the FSR target. Only a SCALE (preset)
//           change flips fsr4Ready_. A pure window resize must NOT trigger
//           teardown — that forced a multi-second weight-blob reload + pipeline
//           rebuild on every resize and stalled the UI on vkQueueWaitIdle.
void PlaybackEngine::setFsrViewport(uint32_t width, uint32_t height,
                                    float scale) {
  width = std::max(2u, width);
  height = std::max(2u, height);
  scale = std::max(1.0f, scale);
  if (fsrViewportW_.load() == width && fsrViewportH_.load() == height &&
      fsrScale_.load() == scale)
    return;
  const uint32_t oldWidth = fsrViewportW_.load();
  const uint32_t oldHeight = fsrViewportH_.load();
  const float oldScale = fsrScale_.load();
  fsrViewportW_.store(width);
  fsrViewportH_.store(height);
  fsrScale_.store(scale);
  if (fsrViewportChangedUs_.load(std::memory_order_acquire) == 0)
    fsrViewportChangedUs_.store(1, std::memory_order_release);
  else {
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    fsrViewportChangedUs_.store(nowUs, std::memory_order_release);
  }
  // spec 02: window size only affects presentation, never the FSR target.
  // Only a scale (preset) change requires the decode loop to re-initialize
  // the harness with new output dimensions. A pure window resize must NOT
  // trigger teardown — that would force a multi-second weight-blob reload
  // + pipeline rebuild on every resize and stall the UI on vkQueueWaitIdle.
  if (scale != oldScale || width != oldWidth || height != oldHeight) {
    if (scale != oldScale) {
      fsrTemporalResetRequested_.store(true, std::memory_order_release);
      fsrTargetViewportW_.store(width, std::memory_order_release);
      fsrTargetViewportH_.store(height, std::memory_order_release);
      fsr4Ready_.store(false, std::memory_order_release);
      fsr4FrameReady_.store(false, std::memory_order_release);
    } else {
      // Window drags are debounced by promoteStableFsrViewport().
      const uint32_t targetW = fsrTargetViewportW_.load();
      const uint32_t targetH = fsrTargetViewportH_.load();
      const uint64_t requestedArea = static_cast<uint64_t>(width) * height;
      const uint64_t targetArea = static_cast<uint64_t>(targetW) * targetH;
      // DRS hysteresis: require a larger excursion before increasing the
      // render target than before decreasing it.  Window-manager resize
      // events often straddle a pixel boundary; a symmetric 5% threshold
      // would repeatedly tear down/recreate the FSR target there.
      const bool growing = requestedArea > targetArea;
      const uint64_t excursion = growing
          ? requestedArea - targetArea
          : targetArea - requestedArea;
      const uint64_t threshold = growing
          ? std::max<uint64_t>(targetArea / 10u, 64u * 64u)
          : std::max<uint64_t>(targetArea / 14u, 48u * 48u);
      if (excursion >= threshold) {
        fsrTargetViewportW_.store(width, std::memory_order_release);
        fsrTargetViewportH_.store(height, std::memory_order_release);
      }
    }
  }
}

// fsr4NativeOutput: hand the Qt render thread the FSR4-upscaled output image.
//
// Called by: VideoSurfaceItem::updatePaintNode on the Qt render thread (~60Hz).
// Calls:     reads fsr4Enabled_/fsr4Ready_/fsr4FrameReady_ atomics, fsr4Uploader_.
// Notes:     CRITICAL INVARIANT (regression lesson 2026-07-21): deliberately
//            does NOT take fsrDispatchMutex_. Holding it serialized every render
//            frame against every FSR4 dispatch (5+ms for 1080p->4K) = stutter.
//            Teardown safety comes from teardownFsr4Path()'s vkQueueWaitIdle()
//            retiring in-flight render-thread reads before the uploader is freed.
//            The dispatch waits for GPU completion before publishing this handle.
bool PlaybackEngine::fsr4NativeOutput(VkImage &image, uint32_t &width,
                                      uint32_t &height) const {
  // EASU-only mode: return the EASU 2x output image (no FSR4 neural dispatch).
  // The uploader is alive (kept by easuOnlyMode_), so we can hand its EASU
  // image directly to the render thread. Same no-mutex invariant applies.
  if (easuOnlyMode_.load(std::memory_order_acquire)) {
    if (!fsr4FrameReady_.load(std::memory_order_acquire) || !fsr4Uploader_)
      return false;
    if (!fsr4Uploader_->easuReady())
      return false;
    image = fsr4Uploader_->easuColorImage();
    width = fsr4Uploader_->easuW();
    height = fsr4Uploader_->easuH();
    return image != VK_NULL_HANDLE && width > 0 && height > 0;
  }
  if (!fsr4Enabled_.load(std::memory_order_acquire) ||
      !fsr4Ready_.load(std::memory_order_acquire) ||
      !fsr4FrameReady_.load(std::memory_order_acquire))
    return false;
  GpuImageUploader *published =
      fsr4PublishedUploader_.load(std::memory_order_acquire);
  if (!published)
    return false;
  // NOTE: this is called from the Qt render thread at ~60Hz. We deliberately
  // do NOT take fsrDispatchMutex_ here — holding it would serialize every
  // render frame against every FSR4 dispatch (5+ms for 1080p->4K) and cause
  // visible stutter. Teardown safety comes from teardownFsr4Path() calling
  // vkQueueWaitIdle() before destroying the uploader, which retires any
  // in-flight render-thread read of the old VkImage.
  // The dispatch waits for completion before publishing this handle. The
  // output image is the current frame in display space; history is model
  // space and must never be presented as the video frame.
  if (fsr4NativePassthrough_.load(std::memory_order_acquire)) {
    image = published->rawPresentationImage();
    width = published->sourceW();
    height = published->sourceH();
  } else {
    image = published->presentationImage();
    width = published->presentationW();
    height = published->presentationH();
  }
  return image != VK_NULL_HANDLE && width > 0 && height > 0;
}

// fsr4RawOutput: hand the Qt render thread the RAW decoded-frame presentation
//                image (for compare mode / raw-only passthrough).
//
// Called by: VideoSurfaceItem::updatePaintNode on the Qt render thread (~60Hz).
// Calls:     reads fsr4Ready_ atomic, fsr4Uploader_->rawPresentationImage().
// Notes:     Same no-mutex invariant as fsr4NativeOutput (teardown via
//            vkQueueWaitIdle, not locking).
bool PlaybackEngine::fsr4RawOutput(VkImage &image, uint32_t &width,
                                   uint32_t &height) const {
  if (!fsr4Ready_.load(std::memory_order_acquire))
    return false;
  GpuImageUploader *published =
      fsr4PublishedUploader_.load(std::memory_order_acquire);
  if (!published)
    return false;
  image = published->rawPresentationImage();
  width = published->sourceW();
  height = published->sourceH();
  return image != VK_NULL_HANDLE && width > 0 && height > 0;
}

qint64 PlaybackEngine::durationUs() const {
  std::lock_guard lock(infoMutex_);
  return durationUs_;
}

qint64 PlaybackEngine::positionUs() const {
  // Prefer audio clock (spec 01 primary clock).
  int64_t clk = audio_.clockUs();
  if (clk >= 0)
    return clk;
  return lastRenderedPtsUs_.load(std::memory_order_acquire);
}

QString PlaybackEngine::mediaTitle() const {
  std::lock_guard lock(infoMutex_);
  return mediaTitle_;
}

QVariantMap PlaybackEngine::mediaInfoQml() const {
  std::lock_guard lock(infoMutex_);
  return mediaInfoQml_;
}

int PlaybackEngine::volume() const { return volume_.load(); }
void PlaybackEngine::setVolume(int v) {
  v = std::clamp(v, 0, 100);
  volume_.store(v);
  audio_.setVolume(v / 100.0f);
  emit volumeChanged();
}
bool PlaybackEngine::muted() const { return muted_.load(); }
void PlaybackEngine::setMuted(bool m) {
  muted_.store(m);
  audio_.setMuted(m);
  emit volumeChanged();
}

// setCompareEnabled: toggle split-screen A/B compare mode.
//
// Called by: QML compare property (Q_PROPERTY write), from the UI thread.
// Notes:     CRITICAL INVARIANT (regression lesson 2026-07-20): does NOT touch
//            fsr4Uploader_ here. The decode loop reads compareEnabled_ and
//            forwards it to the uploader under fsrDispatchMutex_ on the next
//            frame. Reading the unique_ptr from the UI thread would race
//            teardownFsr4Path() which can reset it on a preset/file change.
void PlaybackEngine::setCompareEnabled(bool enabled) {
  if (compareEnabled_.exchange(enabled, std::memory_order_acq_rel) == enabled)
    return;
  // Do NOT touch fsr4Uploader_ here: the decode loop reads compareEnabled_
  // and forwards it to the uploader under fsrDispatchMutex_ on the next
  // frame. Reading the unique_ptr from the UI thread would race
  // teardownFsr4Path() which can reset it on a preset/file change.
  emit compareEnabledChanged();
}

QString PlaybackEngine::playlistEntry(const QUrl &url) {
  if (url.isLocalFile())
    return QFileInfo(url.toLocalFile()).absoluteFilePath();
  return url.toString();
}

QUrl PlaybackEngine::playlistUrl(const QString &entry) {
  const QUrl parsed(entry);
  if (parsed.isValid() && !parsed.scheme().isEmpty())
    return parsed;
  const QFileInfo file(entry);
  if (file.exists())
    return QUrl::fromLocalFile(file.absoluteFilePath());
  return QUrl::fromUserInput(entry);
}

void PlaybackEngine::openUrl(const QUrl &url) {
  const QString entry = playlistEntry(url);
  playlist_ = entry.isEmpty() ? QStringList{} : QStringList{entry};
  playlistIndex_ = entry.isEmpty() ? -1 : 0;
  emit playlistChanged();
  if (entry.isEmpty()) {
    close();
    emit errorOccurred(QStringLiteral("No media file was selected"));
    return;
  }
  (void)openUrlInternal(playlistUrl(entry));
}

void PlaybackEngine::openPlaylist(const QStringList &entries) {
  QStringList normalized;
  for (const QString &entry : entries) {
    if (entry.trimmed().isEmpty())
      continue;
    const QString normalizedEntry = playlistEntry(playlistUrl(entry));
    if (!normalizedEntry.isEmpty())
      normalized.push_back(normalizedEntry);
  }

  if (normalized.isEmpty()) {
    clearPlaylist();
    emit errorOccurred(QStringLiteral("The playlist is empty"));
    return;
  }

  playlist_ = normalized;
  playlistIndex_ = 0;
  emit playlistChanged();
  (void)openUrlInternal(playlistUrl(playlist_.first()));
}

void PlaybackEngine::appendPlaylist(const QStringList &entries) {
  QStringList normalized;
  for (const QString &entry : entries) {
    if (entry.trimmed().isEmpty())
      continue;
    const QString normalizedEntry = playlistEntry(playlistUrl(entry));
    if (!normalizedEntry.isEmpty())
      normalized.push_back(normalizedEntry);
  }
  if (normalized.isEmpty())
    return;

  const bool wasEmpty = playlist_.isEmpty();
  playlist_.append(normalized);
  if (wasEmpty) {
    playlistIndex_ = 0;
    emit playlistChanged();
    (void)openUrlInternal(playlistUrl(playlist_.first()));
  } else {
    emit playlistChanged();
  }
}

bool PlaybackEngine::openPlaylistIndex(int index) {
  if (index < 0 || index >= playlist_.size())
    return false;
  playlistIndex_ = index;
  emit playlistChanged();
  return openUrlInternal(playlistUrl(playlist_.at(index)));
}

void PlaybackEngine::selectPlaylist(int index) {
  if (index < 0 || index >= playlist_.size())
    return;
  if (index == playlistIndex_ && hasMedia()) {
    play();
    return;
  }
  (void)openPlaylistIndex(index);
}

void PlaybackEngine::next() {
  if (!hasNext())
    return;
  for (int index = playlistIndex_ + 1; index < playlist_.size(); ++index) {
    if (openPlaylistIndex(index))
      return;
  }
}

void PlaybackEngine::previous() {
  if (!hasMedia())
    return;
  if (positionUs() > 3'000'000) {
    seekUs(0);
    return;
  }
  if (hasPrevious())
    (void)openPlaylistIndex(playlistIndex_ - 1);
  else
    seekUs(0);
}

void PlaybackEngine::clearPlaylist() {
  playlist_.clear();
  playlistIndex_ = -1;
  emit playlistChanged();
  close();
}

// openUrlInternal: open and begin playing one already-selected playlist item.
//
// Called by: openUrl/openPlaylist/appendPlaylist/next/previous/selectPlaylist.
// Calls:     close() (fully stops threads + drains GPU + tears down FSR4 so the
//            next file starts clean), Demuxer::open, VideoDecoder/AudioDecoder::open,
//            startThreads, AudioSink::start.
// Notes:     Regression lesson 2026-07-20: close() alone is enough — do NOT also
//            call stopThreads() here (it re-joined already-joined threads).
//            Emits errorOccurred on open failure; mediaChanged/stateChanged on success.
bool PlaybackEngine::openUrlInternal(const QUrl &url) {
  logInfo("PlaybackEngine: openUrl('{}')", url.toString().toStdString());
  // close() fully stops the decode threads, drains the GPU queue, tears
  // down the FSR4 harness/uploader, and resets all media state. Calling
  // stopThreads() again here used to join threads that were already
  // joined and could race a half-shutdown decode loop. close() is enough.
  close();
  logInfo("PlaybackEngine: close() done, opening demuxer");

  std::string path;
  if (url.isLocalFile())
    path = url.toLocalFile().toStdString();
  else
    path = url.toString().toStdString();

  demux_ = std::make_unique<Demuxer>();
  logInfo("PlaybackEngine: demux->open('{}')", path);
  if (!demux_->open(path)) {
    emit errorOccurred(QString("Could not open file: ") + url.toString());
    return false;
  }
  logInfo("PlaybackEngine: demux opened ok");

  const auto &info = demux_->info();
  {
    std::lock_guard lock(infoMutex_);
    durationUs_ = info.durationUs;
    // Derive a display title from the filename.
    QString name = QString::fromStdString(info.url);
    int slash = name.lastIndexOf('/');
    if (slash >= 0)
      name = name.mid(slash + 1);
    mediaTitle_ = name;

    QVariantMap m;
    m["fileName"] = name;
    m["path"] = QString::fromStdString(info.url);
    m["container"] = QString::fromStdString(info.container);
    m["durationUs"] = static_cast<qint64>(info.durationUs);
    if (info.video) {
      m["videoCodec"] = QString::fromStdString(info.video->codecName);
      m["width"] = info.video->width;
      m["height"] = info.video->height;
      m["fps"] = info.video->frameRate;
      srcW_ = info.video->width;
      srcH_ = info.video->height;
    }
    if (info.audio) {
      m["audioCodec"] = QString::fromStdString(info.audio->codecName);
      m["sampleRate"] = info.audio->sampleRate;
      m["channels"] = info.audio->channels;
    }
    mediaInfoQml_ = m;
  }

  if (info.videoIndex >= 0) {
    vdec_ = std::make_unique<VideoDecoder>();
    // Resolve typed motion configuration before opening the decoder. VAAPI
    // frames remain excellent for ordinary playback, but that handoff does
    // not preserve FFmpeg motion side-data. A configured causal estimator
    // therefore needs to request software decode at this boundary; the
    // estimator itself still runs later on the unjittered decoded pair.
    const MotionMode userMotionMode =
        motionMode_.load(std::memory_order_acquire);
    const bool userNeedsCodecMotion =
        userMotionMode == MotionMode::AutoCheap ||
        userMotionMode == MotionMode::Codec;
    // Capture controls may explicitly disable motion even when the shared
    // Quality Lab profile contains motion settings for the promoted arm.
    // Resolve that override before opening the decoder, otherwise the typed
    // profile forces software decode and FFmpeg side-data extraction for a
    // control that intentionally must not consume either one.
    const char *environmentMotionMode =
        std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR");
    const bool environmentMotionModeIsOff =
        environmentMotionMode &&
        (std::strcmp(environmentMotionMode, "off") == 0 ||
         std::strcmp(environmentMotionMode, "zero") == 0);
    if (!environmentMotionModeIsOff &&
        ((qualityLabConfig_.enabled && qualityLabConfig_.motionConfigured &&
         qualityLabConfig_.motion.mode != MotionEstimatorMode::Off) ||
         userNeedsCodecMotion))
      vdec_->setMotionMetadataRequested(true);
    if (!vdec_->open(demux_->ctx(), info.videoIndex))
      vdec_.reset();
  }
  if (info.audioIndex >= 0) {
    adec_ = std::make_unique<AudioDecoder>();
    if (!adec_->open(demux_->ctx(), info.audioIndex))
      adec_.reset();
  }

  hasMedia_.store(true);
  emit mediaChanged();

  // Start audio device + threads.
  if (adec_) {
    logInfo("PlaybackEngine: starting audio ({}ch {}Hz)", adec_->outChannels(),
            adec_->outSampleRate());
    if (audio_.start(adec_->outChannels(), adec_->outSampleRate())) {
      audio_.setVolume(volume_.load() / 100.0f);
      audio_.setMuted(muted_.load());
      logInfo("PlaybackEngine: audio started");
    } else {
      logWarn("PlaybackEngine: audio device start failed; using PTS clock "
              "fallback");
    }
  }
  logInfo("PlaybackEngine: starting decode threads");
  startThreads();
  playing_.store(true);
  emit stateChanged();
  logInfo("PlaybackEngine: opened '{}', playing={}", path, playing_.load());
  endOfMediaPending_.store(false, std::memory_order_release);
  return true;
}

void PlaybackEngine::advancePlaylistAtEnd() {
  if (!endOfMediaPending_.exchange(false, std::memory_order_acq_rel))
    return;
  if (hasNext()) {
    next();
    return;
  }
  // Keep the last frame visible when the final item ends, but stop the clock
  // so the UI does not continue polling a drained stream forever.
  pause();
}

// play: resume playback (sets playing_=true). Called by QML (Q_INVOKABLE).
//       No-op if no media; emits stateChanged on the true transition.
void PlaybackEngine::play() {
  if (!hasMedia_.load())
    return;
  bool was = playing_.exchange(true);
  if (!was)
    emit stateChanged();
}

// pause: pause playback (spec 01 Pause Handling).
//
// Called by: QML (Q_INVOKABLE).
// Calls:    sets playing_=false, AudioSink::stop (halts the master clock).
// Notes:    Keeps the current output texture displayed and stops advancing/
//           redispatching FSR. The audio device restarts on the next play().
void PlaybackEngine::pause() {
  if (!hasMedia_.load())
    return;
  // spec 01 Pause Handling: stop advancing video frames, keep current
  // output texture displayed, do not repeatedly redispatch FSR.
  bool was = playing_.exchange(false);
  if (was)
    emit stateChanged();
  // Pause audio clock by stopping the device; we'll restart on play.
  // (Simplest correct approach: drain nothing, just halt consumption.)
  audio_.stop();
}

// togglePlay: flip between play and pause. Called by QML (Q_INVOKABLE).
//              On resume, restarts the audio clock at the last known position.
void PlaybackEngine::togglePlay() {
  if (playing_.load())
    pause();
  else {
    if (adec_) {
      // Restart audio clock at the last known position.
      int64_t pos = lastRenderedPtsUs_.load();
      if (pos < 0)
        pos = 0;
      audio_.start(adec_->outChannels(), adec_->outSampleRate());
      audio_.setStartPts(pos);
      audio_.setVolume(volume_.load() / 100.0f);
      audio_.setMuted(muted_.load());
    }
    play();
  }
}

// seekUs: seek to a target time in microseconds.
//
// Called by: QML (Q_INVOKABLE) from the position slider.
// Calls:    sets seekTargetUs_/seekPending_, clears the video/audio packet
//           queues + frame queue + audio ring, notifies the demux/decode threads.
// Notes:    spec 02 Seek Handling — flush queued frames, reset temporal history,
//           set reset=true on the first frame after seek, resume at source
//           timestamps. The demux loop observes seekPending_ and performs the
//           actual Demuxer::seekUs + decoder flush + audio-clock rebase.
void PlaybackEngine::seekUs(qint64 us) {
  if (!hasMedia_.load())
    return;
  // spec 02 Seek Handling: flush queued frames, reset temporal history,
  // set reset=true on first frame after seek, resume at source timestamps.
  seekTargetUs_.store(us);
  seekGeneration_.fetch_add(1, std::memory_order_acq_rel);
  seekPending_.store(true);

  // Wake threads to observe the flush.
  {
    std::lock_guard lock(pktMutex_);
    videoPackets_.clear();
    audioPackets_.clear();
  }
  {
    std::lock_guard lock(frameMutex_);
    frames_.clear();
  }
  {
    std::lock_guard lock(audioMutex_);
    audioChunks_.clear();
    audio_.clear();
  }
  pktCv_.notify_all();
  frameCv_.notify_one();
}

// close: stop playback and tear down all media + GPU state (idempotent).
//
// Called by: openUrl (re-open), the QML stop/closed signal, dtor.
// Calls:     stopThreads, AudioSink::stop, resets demux_/vdec_/adec_,
//            teardownFsr4Path (waits for GPU queue to idle first so the Qt
//            render thread stops referencing the old images).
// Notes:     Clears seekPending_ (was leaking into the next file) and resets
//            mediaInfo_/srcW_/srcH_ under infoMutex_. Emits mediaChanged/stateChanged.
void PlaybackEngine::close() {
  hasMedia_.store(false);
  playing_.store(false);
  endOfMediaPending_.store(false, std::memory_order_release);
  logInfo("PlaybackEngine: close() stopThreads");
  stopThreads();
  logInfo("PlaybackEngine: close() audio.stop");
  audio_.stop();
  if (demux_)
    demux_->requestAbort();
  {
    std::lock_guard lock(pktMutex_);
    videoPackets_.clear();
    audioPackets_.clear();
  }
  {
    std::lock_guard lock(frameMutex_);
    frames_.clear();
  }
  {
    std::lock_guard lock(audioMutex_);
    audioChunks_.clear();
  }
  // Clear any stale seek flag so a freshly opened file does not begin
  // life mid-seek against a brand-new demuxer.
  seekPending_.store(false);
  queuedFrames_.store(0);
  vdec_.reset();
  adec_.reset();
  demux_.reset();
  // Drop the live FSR4 harness/uploader so the next file starts from a
  // clean Vulkan state. teardownFsr4Path waits for the GPU queue to idle
  // first, which retires any image still held by the Qt render thread.
  teardownFsr4Path();
  lastRenderedPtsUs_.store(-1);
  lastAnalysisPtsUs_ = -1;
  lastFramePts_ = -1;
  lastFrameWallTime_ = {};
  {
    std::lock_guard lock(infoMutex_);
    mediaInfoQml_.clear();
    mediaTitle_.clear();
    durationUs_ = 0;
    srcW_ = srcH_ = 0;
  }
  emit mediaChanged();
  emit stateChanged();
  logInfo("PlaybackEngine: close() fully done");
}

// startThreads: launch the demux + video-decode + audio-decode threads.
//                Called by: openUrl / play once decoders are open. Sets running_=true.
//                Notes: each thread checks running_ as its loop condition.
void PlaybackEngine::startThreads() {
  running_.store(true);
  demuxThread_ = std::thread([this] { demuxLoop(); });
  if (vdec_)
    videoThread_ = std::thread([this] { videoDecodeLoop(); });
  if (adec_)
    audioThread_ = std::thread([this] { audioDecodeLoop(); });
}

// stopThreads: signal the threads to stop and join them.
//
// Called by: close / dtor / openUrl path. Called from the UI thread.
// Calls:    sets running_=false, Demuxer::requestAbort, notifies pktCv_/frameCv_
//           (to wake any blocked demux/decode), joins all three threads.
// Notes:    Must run AFTER any in-flight dispatch completes — the decode thread
//           does synchronous vkWaitForFences(UINT64_MAX) per frame.
void PlaybackEngine::stopThreads() {
  running_.store(false);
  if (demux_)
    demux_->requestAbort();
  pktCv_.notify_all();
  frameCv_.notify_one();
  if (demuxThread_.joinable())
    demuxThread_.join();
  if (videoThread_.joinable())
    videoThread_.join();
  if (audioThread_.joinable())
    audioThread_.join();
}

// demuxLoop: the playback/demux thread — reads packets and routes them to the
//            per-stream packet queues.
//
// Runs on:  demuxThread_ (started by startThreads). Exits when running_ is false.
// Calls:    handles pending seeks (VideoDecoder/AudioDecoder::flush + Demuxer::seekUs
//           + AudioSink::setStartPts to rebase the clock), Demuxer::readPacket,
//           pushes to videoPackets_/audioPackets_ under pktMutex_ with pktCv_.
// Notes:    On EOF queues exactly one null-packet per decoder to drain delayed frames;
//           then waits idle until a seek/close re-arms the loop.
void PlaybackEngine::demuxLoop() {
  while (running_.load()) {
    // Handle pending seek.
    if (seekPending_.exchange(false)) {
      int64_t target = seekTargetUs_.load();
      if (vdec_)
        vdec_->flush();
      if (adec_)
        adec_->flush();
      demux_->seekUs(target);
      audio_.setStartPts(target); // rebase audio clock at seek target
      lastRenderedPtsUs_.store(target);
    }

    Packet pkt;
    if (!demux_->readPacket(pkt)) {
      // Queue exactly one EOF marker per decoder. A null packet drains delayed
      // codec frames; flushing here would discard them.
      auto enqueueEof = [&](auto &queue, size_t capacity) {
        std::unique_lock lock(pktMutex_);
        pktCv_.wait(lock, [&] {
          return !running_.load() || seekPending_.load() ||
                 queue.size() < capacity;
        });
        if (!running_.load() || seekPending_.load())
          return;
        Packet eof;
        eof.isEof = true;
        queue.push_back(std::move(eof));
        lock.unlock();
        pktCv_.notify_all();
      };
      if (vdec_)
        enqueueEof(videoPackets_, kMaxVideoPackets);
      if (adec_ && running_.load() && !seekPending_.load())
        enqueueEof(audioPackets_, kMaxAudioPackets);

      // EOF is terminal until a seek or close. Waiting here prevents repeated
      // drain markers and keeps the demux thread idle.
      std::unique_lock lock(pktMutex_);
      pktCv_.wait(lock, [&] {
        return !running_.load() || seekPending_.load();
      });
      continue;
    }

    int si = pkt.streamIndex;
    std::unique_lock lock(pktMutex_);
    if (vdec_ && si == vdec_->streamIndex()) {
      pktCv_.wait(lock, [&] {
        return !running_.load() || seekPending_.load() ||
               videoPackets_.size() < kMaxVideoPackets;
      });
      if (running_.load() && !seekPending_.load())
        videoPackets_.push_back(std::move(pkt));
    } else if (adec_ && si == adec_->streamIndex()) {
      pktCv_.wait(lock, [&] {
        return !running_.load() || seekPending_.load() ||
               audioPackets_.size() < kMaxAudioPackets;
      });
      if (running_.load() && !seekPending_.load())
        audioPackets_.push_back(std::move(pkt));
    }
    lock.unlock();
    pktCv_.notify_all();
  }
}

// videoDecodeLoop: the video decode thread — pulls packets, decodes frames,
//                  runs the FSR4 dispatch, and publishes displayable frames.
//
// Runs on:  videoThread_ (started by startThreads). Exits when running_ is false.
// Calls:    VideoDecoder::sendPacket/receiveFrame, GpuImageUploader::upload,
//           Fsr4DispatchHarness::dispatchFrame (the synchronous per-frame
//           vkWaitForFences(UINT64_MAX) — anything stopping this thread must
//           let the current dispatch finish), pushes VideoFrameForRender to the
//           render queue under frameCv_, handles fsrAbortRequested_ between frames.
// Notes:    This is where the load-bearing dispatch happens; fsrDispatchMutex_
//           is held only around the dispatch itself, never around the whole loop.
void PlaybackEngine::videoDecodeLoop() {
  // A newly opened decoder has no published history image yet. The first
  // decoded frame must take the reset path even without an explicit flush.
  bool firstAfterSeek = true;
  uint64_t handledSeekGeneration =
      seekGeneration_.load(std::memory_order_acquire);
  // The last jitter that belongs to a successfully submitted history image.
  // This is passed to the prepass so temporal reprojection can align jittered
  // source phases instead of treating a static subpixel shift as motion.
  bool hasPreviousJitter = false;
  float previousJitterX = 0.0f;
  float previousJitterY = 0.0f;
  // Prior jitter is expressed in the decoded render-size coordinate space.
  // Keep the dimensions that produced it so a source-size change cannot
  // reuse those values in a different pixel-unit space.
  uint32_t previousRenderWidth = 0;
  uint32_t previousRenderHeight = 0;
  // Tracks only successfully completed FSR dispatches. This prevents a
  // skipped/reordered decoder frame or failed GPU submission from consuming
  // history/recurrent state belonging to a different frame.
  TemporalFrameContinuity temporalFrameContinuity;
  DecodedVideoFrame pendingDecodedFrame;
  bool hasPendingDecodedFrame = false;
  static const bool forceResetEnv =
      std::getenv("TFORGE_FSR4_FORCE_RESET") != nullptr;
  static const bool motionConfidenceReactiveEnv =
      std::getenv("TFORGE_FSR4_MOTION_CONFIDENCE_REACTIVE") != nullptr;
  // The promoted best-findings temporal path enables only already-measured
  // helpers: codec-motion confidence annotation and strict validation. The
  // bounded CPU refinement probe was not a quality win and added measurable
  // frame time, so it remains explicitly selectable for diagnostics only.
  // Upstream: decoded frames and codec motion. Downstream: FSR temporal
  // history/recurrent inputs. FSR1/EASU remains a separate opt-in because its
  // matched evidence did not show a safe universal improvement.
  static const bool bestFindingsTemporal =
      std::getenv("TFORGE_FSR4_DISABLE_BEST_FINDINGS") == nullptr;
  // Integrated video profile: combine only retained causal motion/history
  // findings as one reproducible arm. Source-tap Mu-law ordering remains a
  // separate diagnostic because its matched result was neutral. Synthetic
  // jitter is also separate because zero jitter remains the video default.
  static const bool integratedTemporalProfile =
      std::getenv("TFORGE_FSR4_INTEGRATED_TEMPORAL") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") != nullptr;
  // Explicit comparison arm that combines the measured best-findings stack
  // with synthetic jitter. It is intentionally separate from the promoted
  // zero-jitter profile because prerecorded frames do not contain renderer
  // jitter samples; the capture harness can therefore A/B this combination
  // without changing the baseline or silently altering ordinary playback.
  static const bool integratedBestFindingsJitter =
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") != nullptr;
  // This opt-in adds the measured history/confidence candidate to the
  // integrated motion/color profile while preserving the base profile.
  static const bool integratedHistoryConfidenceProfile =
      std::getenv("TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS") != nullptr ||
      std::getenv("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER") != nullptr;
  // The integrated causal profile must not use a B-picture's past reference
  // list as though it were the immediately previous displayed frame. Keep
  // this guard opt-in so existing motion A/B controls remain reproducible.
  // FFmpeg's past-reference sign does not identify the immediately previous
  // displayed frame for every B-picture stream. Keep rejection opt-in until
  // a broader capture set proves it is a net gain; this switch is a diagnostic
  // A/B control, not a new default. Upstream: decoder picture type and runtime
  // selection. Downstream: causal history seeds and dense motion expansion.
  static const bool rejectBFrameMotion =
      std::getenv("TFORGE_FSR4_MOTION_ALLOW_B_FRAMES") == nullptr &&
      std::getenv("TFORGE_FSR4_MOTION_REJECT_B_FRAMES") != nullptr;
  static const char *jitterModeEnv = std::getenv("TFORGE_FSR4_JITTER_MODE");
  // Future-frame probes need one decoded lookahead frame. The ordinary path
  // remains packet-for-packet causal and does not drain another video packet.
  static const bool futureLookaheadEnv =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_BIDIRECTIONAL_MOTION") != nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY") != nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_DISPLAY_INTERPOLATED") != nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_INTERPOLATED_JITTER") != nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_FUTURE_ALIGNED_JITTER") != nullptr;
  static const bool displayInterpolatedEnv =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_DISPLAY_INTERPOLATED") != nullptr;
  // This variant uses the synthesized midpoint as the reconstruction sample
  // that replaces artificial render jitter, but keeps one output per decoded
  // frame and preserves that frame's presentation timestamp.
  static const bool interpolatedJitterEnv =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_INTERPOLATED_JITTER") != nullptr;
  static const bool futureAlignedJitterEnv =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_FUTURE_ALIGNED_JITTER") != nullptr;
  // Future-aligned/interpolated probes intentionally replace synthetic jitter
  // with their own sample. The upload pass and FSR constants must agree.
  static const bool syntheticJitterApplied =
      !interpolatedJitterEnv && !futureAlignedJitterEnv;
  // Opt-in RE-guided ordering probe. The official FSR source applies jitter
  // inside its prepass input resolve after model-color conversion, whereas
  // the established video path applies it during YUV upload. Keep motion
  // estimation and FSR metadata unchanged; only move the color sample phase.
  static const bool prepassJitterOrdering =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_PREPASS_JITTER_ORDERING") != nullptr ||
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW") != nullptr ||
      integratedBestFindingsJitter;
  // Optional photometric rejection keeps a future-aligned sample out of
  // disoccluded or badly matched pixels. A negative value disables this gate
  // so the original future-aligned averaging candidate remains reproducible.
  static const float futureAlignPhotometricThreshold = [] {
    const char *value = std::getenv(
        "TFORGE_FSR4_FUTURE_ALIGN_PHOTOMETRIC_THRESHOLD");
    if (!value || !*value)
      return -1.0f;
    char *end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value && *end == '\0' && std::isfinite(parsed)
               ? std::clamp(parsed, 0.0f, 1.0f)
               : -1.0f;
  }();
  static const bool interpolationProbeEnv =
      displayInterpolatedEnv || interpolatedJitterEnv ||
      futureAlignedJitterEnv;
  static const char *interpolationMotionPath = std::getenv(
      "TFORGE_FSR4_EXPERIMENTAL_INTERPOLATION_DENSE_MOTION");
  static const float controlledJitterStrength = [] {
    const char *value = std::getenv("TFORGE_FSR4_CONTROLLED_JITTER");
    return value ? std::clamp(std::strtof(value, nullptr), 0.0f, 1.5f) : 1.0f;
  }();
  static const char *jitterSequenceEnv =
      std::getenv("TFORGE_FSR4_JITTER_SEQUENCE");
  static const bool fullAmplitudeJitterEnv =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_FULL_JITTER") != nullptr;
  // Keep an opt-in sign probe at the single boundary where synthetic jitter
  // is converted from the authored source-pixel sample into the color-input
  // and FSR metadata values. Both consumers must receive the same sign; the
  // motion field remains unjittered. The default preserves the established
  // positive convention, while the negative arm lets a capture disambiguate
  // coordinate-orientation mistakes without changing normal playback.
  static const float jitterSign = [] {
    const char *value = std::getenv("TFORGE_FSR4_JITTER_SIGN");
    return value && std::strcmp(value, "negative") == 0 ? -1.0f : 1.0f;
  }();
  // Diagnostic-only relative-sign probe. Unlike jitterSign, this changes only
  // the physical color sample while leaving the FSR-reported metadata on the
  // selected convention. It intentionally creates an A/B mismatch so the
  // capture can determine whether the source sampling orientation, rather
  // than the whole convention, is responsible for a quality loss.
  static const float jitterSampleSign = [] {
    const char *value = std::getenv("TFORGE_FSR4_JITTER_SAMPLE_SIGN");
    return value && std::strcmp(value, "negative") == 0 ? -1.0f : 1.0f;
  }();
  static const uint32_t jitterCadence = [] {
    const char *value = std::getenv("TFORGE_FSR4_JITTER_CADENCE");
    if (!value || !*value)
      return 1u;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0')
      return 1u;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 1ul, 64ul));
  }();
  static const bool dumpDecoderEnv =
      std::getenv("TFORGE_FSR4_DUMP_DECODER") != nullptr;
  static const uint32_t dumpDecoderFrame = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_DECODER_FRAME");
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static const bool dumpOutputEnv =
      std::getenv("TFORGE_FSR4_DUMP_OUTPUT") != nullptr;
  static const bool dumpPresentationEnv =
      std::getenv("TFORGE_FSR4_DUMP_PRESENTATION") != nullptr;
  static const bool dumpRawEnv = std::getenv("TFORGE_FSR4_DUMP_RAW") != nullptr;
  static const bool dumpModelInputEnv =
      std::getenv("TFORGE_FSR4_DUMP_MODEL_INPUT") != nullptr;
  static const uint32_t dumpModelInputFrame = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_MODEL_INPUT_FRAME");
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static const char *dumpStageDirectory = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_STAGE_DIR");
    return value && *value ? value : "";
  }();
  static const uint32_t dumpOutputFrame = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_OUTPUT_FRAME");
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static const char *dumpOutputPath = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_OUTPUT_PATH");
    return value && *value ? value : "/tmp/temporal_forge_fsr4_output.ppm";
  }();
  static const char *dumpPresentationPath = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_PRESENTATION_PATH");
    return value && *value ? value : "/tmp/temporal_forge_fsr4_presentation.ppm";
  }();
  static const char *dumpRawPath = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_RAW_PATH");
    return value && *value ? value : "/tmp/temporal_forge_fsr4_raw.ppm";
  }();
  static const bool headlessBenchmarkEnv =
      std::getenv("TFORGE_HEADLESS_BENCHMARK") != nullptr;
  static const bool profileUploadEnv =
      std::getenv("TFORGE_FSR4_PROFILE_UPLOAD") != nullptr;
  static const bool profileTimingsEnv =
      std::getenv("TFORGE_FSR4_PROFILE_TIMINGS") != nullptr;
  static const uint32_t fsrLogInterval = [] {
    const char *value = std::getenv("TFORGE_FSR4_LOG_INTERVAL");
    if (!value)
      return 60u;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return static_cast<uint32_t>(std::max(1ul, parsed));
  }();
  static const long dumpSequenceLimit = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_SEQUENCE");
    if (!value)
      return 0l;
    return std::strtol(value, nullptr, 10);
  }();
  static const uint32_t dumpSequenceWarmup = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_SEQUENCE_WARMUP");
    if (!value)
      return 0u;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0')
      return 0u;
    return static_cast<uint32_t>(std::min<unsigned long>(parsed, 100000ul));
  }();
  static const bool dumpMotionSidecarEnv =
      std::getenv("TFORGE_FSR4_DUMP_MOTION_SIDECAR") != nullptr;
  static const bool dumpMotionTextureEnv =
      std::getenv("TFORGE_FSR4_DUMP_MOTION_TEXTURE") != nullptr;
  // Optional output-sized FP16 reprojection readback. Paired with the dense
  // motion dump, it exposes the actual history warp before composition.
  static const bool dumpReprojectedColorEnv =
      std::getenv("TFORGE_FSR4_DUMP_REPROJECTED_COLOR") != nullptr;
  // Optional pre-upload seed tracing records the exact sparse vectors after
  // fallback/refinement/replay selection and source-to-model scaling. This is
  // diagnostic-only: it localizes a bad seed without changing submitted data.
  static const bool dumpMotionSeedsEnv =
      std::getenv("TFORGE_FSR4_DUMP_MOTION_SEEDS") != nullptr;
  static const bool dumpEventTraceEnv =
      std::getenv("TFORGE_FSR4_DUMP_EVENT_TRACE") != nullptr;
  // Synthetic jitter is useful for controlled video experiments, but a decoded
  // video frame cannot expose the renderer samples that real camera jitter
  // would have produced. Keep normal playback on zero jitter unless the caller
  // explicitly opts into a jitter mode; the selected mode belongs in the
  // capture manifest so experimental results remain reproducible.
  if (jitterModeEnv && std::strcmp(jitterModeEnv, "off") == 0)
    sideBufferSynth_.setJitterMode(JitterMode::Off);
  else if (jitterModeEnv && std::strcmp(jitterModeEnv, "reduced") == 0)
    sideBufferSynth_.setJitterMode(JitterMode::Reduced);
  else if (jitterModeEnv && std::strcmp(jitterModeEnv, "controlled") == 0) {
    sideBufferSynth_.setJitterMode(JitterMode::Controlled);
    sideBufferSynth_.setControlledJitterStrength(controlledJitterStrength);
  } else if (jitterModeEnv &&
             (std::strcmp(jitterModeEnv, "synthetic") == 0 ||
              std::strcmp(jitterModeEnv, "current") == 0)) {
    sideBufferSynth_.setJitterMode(JitterMode::Current);
  } else {
    // The default is zero jitter. An absent or unknown selector must not
    // silently enable a temporal sampling experiment in production playback.
    sideBufferSynth_.setJitterMode(JitterMode::Off);
  }
  // The profile selects synthetic Halton jitter only when the caller did not
  // provide an explicit jitter policy. This keeps `JITTER_MODE=off` usable as
  // the matched control while making the combined profile self-contained.
  if (integratedBestFindingsJitter &&
      !(jitterModeEnv && *jitterModeEnv))
    sideBufferSynth_.setJitterMode(JitterMode::Current);
  // The default remains Halton(2,3), one new sample per frame. The other
  // deterministic sequences/cadences are capture-only probes and do not
  // create future-frame or optical-flow dependencies.
  if (jitterSequenceEnv && std::strcmp(jitterSequenceEnv, "halton32") == 0)
    sideBufferSynth_.setJitterSequence(JitterSequence::Halton32);
  else if (jitterSequenceEnv && std::strcmp(jitterSequenceEnv, "alternating") == 0)
    sideBufferSynth_.setJitterSequence(JitterSequence::Alternating);
  else if (jitterSequenceEnv && std::strcmp(jitterSequenceEnv, "zero") == 0)
    sideBufferSynth_.setJitterSequence(JitterSequence::Zero);
  else
    sideBufferSynth_.setJitterSequence(JitterSequence::Halton23);
  sideBufferSynth_.setJitterCadence(jitterCadence);
  sideBufferSynth_.setFullAmplitudeJitter(fullAmplitudeJitterEnv);
  sideBufferSynth_.setMotionConfidenceReactive(motionConfidenceReactiveEnv);
  static const char *dumpSequenceDirectory = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_SEQUENCE_DIR");
    return value && *value ? value : "/tmp";
  }();
  static const char *dumpMotionDirectory = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_MOTION_DIR");
    return value && *value ? value : "/tmp";
  }();
  static const char *dumpEventTraceDirectory = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_EVENT_DIR");
    return value && *value ? value : "/tmp";
  }();
  while (running_.load()) {
    Packet pkt;
    {
      std::unique_lock lock(pktMutex_);
      pktCv_.wait(lock, [&] {
        return !running_.load() || seekPending_.load() ||
               !videoPackets_.empty();
      });
      if (!running_.load())
        break;
      if (seekPending_.load()) {
        // Wait for demux loop to finish flushing.
        continue;
      }
      if (videoPackets_.empty())
        continue;
      pkt = std::move(videoPackets_.front());
      videoPackets_.pop_front();
    }
    pktCv_.notify_all();

    if (pkt.isFlush) {
      vdec_->flush();
      // Keep CPU-side motion analysis paired with the same flush boundary as
      // FSR history. Otherwise the next source frame could compare against a
      // luma frame from the previous seek/file sequence.
      sideBufferSynth_.resetAnalysisHistory();
      pendingDecodedFrame = {};
      hasPendingDecodedFrame = false;
      firstAfterSeek = true;
      hasPreviousJitter = false;
      previousJitterX = 0.0f;
      previousJitterY = 0.0f;
      temporalFrameContinuity.clear();
      handledSeekGeneration =
          seekGeneration_.load(std::memory_order_acquire);
      continue;
    }

    vdec_->sendPacket(pkt.isEof ? nullptr : pkt.av);
    DecodedVideoFrame df;
    double decodeCpuMs = 0.0;
    while (true) {
      // Demux performs the decoder flush on its own thread, so the decode
      // thread uses the generation counter to reset its CPU-side temporal
      // companion state at the same boundary. A pending seek also stops this
      // old packet from producing another frame before the flush completes.
      const uint64_t currentSeekGeneration =
          seekGeneration_.load(std::memory_order_acquire);
      if (seekPending_.load(std::memory_order_acquire)) {
        pendingDecodedFrame = {};
        hasPendingDecodedFrame = false;
        break;
      }
      if (currentSeekGeneration != handledSeekGeneration) {
        sideBufferSynth_.resetAnalysisHistory();
        pendingDecodedFrame = {};
        hasPendingDecodedFrame = false;
        firstAfterSeek = true;
        hasPreviousJitter = false;
        previousJitterX = 0.0f;
        previousJitterY = 0.0f;
        previousRenderWidth = 0;
        previousRenderHeight = 0;
        lastAnalysisPtsUs_ = -1;
        temporalFrameContinuity.clear();
        handledSeekGeneration = currentSeekGeneration;
      }
      if (hasPendingDecodedFrame) {
        // A pending frame may have been copied out of the decoder just before
        // a seek request. It belongs to the old source sequence and must be
        // discarded before any new frame can consume temporal history.
        if (seekPending_.load(std::memory_order_acquire)) {
          pendingDecodedFrame = {};
          hasPendingDecodedFrame = false;
          break;
        }
        df = std::move(pendingDecodedFrame);
        pendingDecodedFrame = {};
        hasPendingDecodedFrame = false;
      } else {
        const auto decodeStart = std::chrono::steady_clock::now();
        if (!vdec_->receiveFrame(df))
          break;
        decodeCpuMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - decodeStart)
                          .count();
      }
      const uint32_t currentRenderWidth =
          static_cast<uint32_t>(std::max(0, df.width));
      const uint32_t currentRenderHeight =
          static_cast<uint32_t>(std::max(0, df.height));
      const bool renderSizeChanged =
          previousRenderWidth != 0 &&
          (previousRenderWidth != currentRenderWidth ||
           previousRenderHeight != currentRenderHeight);
      if (renderSizeChanged) {
        // A resize invalidates both temporal history and the prior jitter
        // coordinate basis. The FSR reset path handles the images; clearing
        // this flag prevents the prepass from subtracting an old-size phase.
        hasPreviousJitter = false;
        previousJitterX = 0.0f;
        previousJitterY = 0.0f;
      }
      previousRenderWidth = currentRenderWidth;
      previousRenderHeight = currentRenderHeight;
      if (futureLookaheadEnv && !hasPendingDecodedFrame) {
        // A decoder generally cannot return the next frame until another
        // packet has been submitted. Pull only video packets here, because
        // the demux thread keeps audio in a separate queue. Flush markers
        // are put back so a seek cannot be consumed as future evidence.
        while (running_.load() && !seekPending_.load()) {
          DecodedVideoFrame nextDecodedFrame;
          if (vdec_->receiveFrame(nextDecodedFrame)) {
            pendingDecodedFrame = std::move(nextDecodedFrame);
            hasPendingDecodedFrame = true;
            break;
          }
          Packet lookaheadPacket;
          {
            std::lock_guard lock(pktMutex_);
            if (videoPackets_.empty()) break;
            lookaheadPacket = std::move(videoPackets_.front());
            videoPackets_.pop_front();
          }
          pktCv_.notify_all();
          if (lookaheadPacket.isFlush) {
            std::lock_guard lock(pktMutex_);
            videoPackets_.push_front(std::move(lookaheadPacket));
            pktCv_.notify_all();
            break;
          }
          vdec_->sendPacket(lookaheadPacket.isEof ? nullptr
                                                  : lookaheadPacket.av);
        }
      } else if (!futureLookaheadEnv) {
        // Preserve the historical cheap receive-only check on the default
        // path. It can observe a decoder-buffered frame without pulling a new
        // packet or changing normal decode scheduling.
        DecodedVideoFrame nextDecodedFrame;
        if (vdec_->receiveFrame(nextDecodedFrame)) {
          pendingDecodedFrame = std::move(nextDecodedFrame);
          hasPendingDecodedFrame = true;
        }
      }
      // First frame after a seek/new-file must reset history (spec 02).
      const bool timestampDiscontinuity =
          lastAnalysisPtsUs_ >= 0 &&
          (df.ptsUs <= lastAnalysisPtsUs_ ||
           df.ptsUs - lastAnalysisPtsUs_ > 5'000'000);
      const bool frameIndexDiscontinuity =
          temporalFrameContinuity.needsReset(df.frameIndex);
      const bool requestedTemporalReset =
          fsrTemporalResetRequested_.exchange(false, std::memory_order_acq_rel);
      // A future-reference vector is only safe after an independently
      // validated future frame. This player remains causal, so timestamp
      // discontinuities reset the temporal state rather than attempting to
      // bridge an uncertain reference chain.
      const bool reset = firstAfterSeek || renderSizeChanged ||
                         timestampDiscontinuity ||
                         frameIndexDiscontinuity || forceResetEnv ||
                         requestedTemporalReset;
      firstAfterSeek = false;

      const int64_t analysisPtsBeforeFrame = lastAnalysisPtsUs_;
      const float ptsDeltaMs = lastAnalysisPtsUs_ >= 0 && df.ptsUs >= lastAnalysisPtsUs_
          ? static_cast<float>(df.ptsUs - lastAnalysisPtsUs_) / 1000.0f
          : 0.0f;
      lastAnalysisPtsUs_ = df.ptsUs;
      promoteStableFsrViewport();
      // JitterOffset is expressed in render/source-pixel units by the FSR
      // contract. The YUV/DRM upload owns the single color sampling offset;
      // the prepass consumes the already-jittered color texture without
      // shifting it a second time. Sizing the policy from the presentation
      // viewport would multiply the phase for an upscale and mis-register
      // color, motion, and history. Keep the upstream jitter policy tied to
      // the decoded frame dimensions; downstream FSR constants still carry
      // the source-to-output scale separately.
      sideBufferSynth_.setRenderSize(
          static_cast<uint32_t>(std::max(0, df.width)),
          static_cast<uint32_t>(std::max(0, df.height)));
      sideBufferSynth_.setPresentationSize(
          fsrTargetViewportW_.load(std::memory_order_acquire),
          fsrTargetViewportH_.load(std::memory_order_acquire));
      // Install the exact render/output pair before update() selects the
      // variable jitter phase. Previously this was done after update(), so
      // the first frame after a resolution/scale change used the old phase
      // count and was then reset for the following frame.
      if (fsr4Enabled_.load(std::memory_order_acquire) &&
          vkDevice_ != VK_NULL_HANDLE) {
        float jitterPairScale = fsrScale_.load(std::memory_order_acquire);
        if (const char *env = std::getenv("TFORGE_FSR4_FORCE_SCALE")) {
          char *end = nullptr;
          const float forced = std::strtof(env, &end);
          if (end != env && std::isfinite(forced) && forced >= 1.0f)
            jitterPairScale = forced;
        }
        const bool jitterPairForcedViewport =
            std::getenv("TFORGE_FSR4_FORCE_VIEWPORT") != nullptr;
        const FsrJitterPair jitterPair = computeFsrJitterPair(
            static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)), jitterPairScale,
            jitterPairForcedViewport,
            fsrViewportW_.load(std::memory_order_acquire),
            fsrViewportH_.load(std::memory_order_acquire),
            fsrTargetViewportW_.load(std::memory_order_acquire),
            fsrTargetViewportH_.load(std::memory_order_acquire));
        sideBufferSynth_.setFsrJitterPair(
            jitterPair.modelW, jitterPair.modelH,
            jitterPair.neuralTargetW, jitterPair.neuralTargetH);
      }
      // Resolve typed Quality Lab motion settings before constructing the
      // analysis pair. The selected refinement mode determines whether the
      // source evidence uses the configured 1/2, 1/4, or 1/8 grid; keeping
      // this decision beside the estimator config prevents a JSON-only motion
      // arm from silently falling back to the legacy analysis width.
      const MotionEstimatorConfig motionEstimatorConfig =
          qualityLabConfig_.motionConfigured
              ? qualityLabConfig_.motion
              : MotionEstimator::configFromEnvironment();
      const MotionMode userMotionMode =
          motionMode_.load(std::memory_order_acquire);
      const bool explicitMotionSelector =
          qualityLabConfig_.motionConfigured ||
          std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR") ||
          std::getenv("TFORGE_FSR4_MOTION_ABLATION");
      MotionEstimatorConfig effectiveMotionConfig = motionEstimatorConfig;
      // A direct estimator selector is an intentional capture/playback
      // override for the estimator mode only. Keep the typed Quality Lab
      // thresholds and budgets intact, but do not let a JSON mode silently
      // replace an explicitly requested `codec`, `refined`, or `off` run.
      // The off case matters for A/B validation: it must really disable the
      // configured estimator instead of falling back to persisted/configured
      // codec motion. The ablation payload labels (`zero`, `block`) remain
      // handled below, after the normal estimator has produced its causal
      // field.
      if (const char *environmentMotionMode =
              std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR");
          environmentMotionMode && *environmentMotionMode) {
        const MotionEstimatorConfig environmentMotionConfig =
            MotionEstimator::configFromEnvironment();
        effectiveMotionConfig.mode = environmentMotionConfig.mode;
        // Keep explicit refinement-policy selectors authoritative too. The
        // typed Quality Lab file supplies reproducible defaults, but a
        // controlled capture must be able to turn edge-aware reconstruction
        // on or off without rewriting that file. This is a policy override;
        // vector direction, units, and FSR descriptors remain unchanged.
        if (std::getenv("TFORGE_FSR4_MOTION_EDGE_AWARE"))
          effectiveMotionConfig.edgeAwareUpscale =
              environmentMotionConfig.edgeAwareUpscale;
        if (std::getenv("TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING"))
          effectiveMotionConfig.allowFallbackAfterFiltering =
              environmentMotionConfig.allowFallbackAfterFiltering;
        // Dense-grid discovery is an explicit diagnostic/capture override.
        // Preserve the typed profile by default, but do not let a JSON motion
        // block suppress a requested sparse-seed coverage experiment.
        if (std::getenv("TFORGE_FSR4_MOTION_DENSE_GRID"))
          effectiveMotionConfig.denseGridFallback =
              environmentMotionConfig.denseGridFallback;
      }
      // Persisted settings are the normal-playback default. Keep explicit
      // Quality Lab/runner selectors authoritative so captures remain
      // reproducible and are not silently changed by a user's saved setting.
      if (!explicitMotionSelector) {
        if (userMotionMode == MotionMode::AutoCheap)
          effectiveMotionConfig.mode = MotionEstimatorMode::CodecRefined;
        else if (userMotionMode == MotionMode::Codec)
          effectiveMotionConfig.mode = MotionEstimatorMode::Codec;
        else
          effectiveMotionConfig.mode = MotionEstimatorMode::Off;
      }
      // Keep this profile atomic: a saved UI motion selector must not turn a
      // combined capture into a different estimator. An explicit benchmark
      // selector still wins so each ablation remains independently testable.
      if (integratedTemporalProfile && !explicitMotionSelector) {
        effectiveMotionConfig.mode = MotionEstimatorMode::CodecRefined;
        effectiveMotionConfig.edgeAwareUpscale = true;
      }
      // The combined profile also covers the case where FFmpeg exposes only
      // unusable reference entries. Keep this opt-in so the established
      // baseline remains a clean control while the quality profile can use a
      // conservative global translation with reduced confidence.
      if ((integratedTemporalProfile || integratedHistoryConfidenceProfile) &&
          !explicitMotionSelector)
        effectiveMotionConfig.allowFallbackAfterFiltering = true;
      // Keep the live scene-cut detector on the same typed threshold as the
      // motion estimator. Upstream: Quality Lab JSON or environment config;
      // downstream: history/jitter reset and confidence gating.
      sideBufferSynth_.setSceneCutThreshold(
          effectiveMotionConfig.sceneCutThreshold);
      const LumaBuffer analysisLuma = makeAnalysisLuma(
          df, effectiveMotionConfig.mode != MotionEstimatorMode::Off);
      std::vector<MvEntry> pastMotion = pastReferenceMotion(
          df.motionVectors, rejectBFrameMotion, df.bFrame);
      // Optional standalone estimator boundary. It consumes the original
      // unjittered luma pair and normalized causal codec seeds, then returns
      // the same source-pixel vectors already understood by the existing GPU
      // expander. With the environment unset, the established path is left
      // untouched; this is an explicit A/B mode for the new subsystem.
      // An explicit Quality Lab motion block is the reproducible source of
      // estimator settings. Files without that block retain the legacy
      // environment parser so existing baseline captures remain unchanged.
      motionEstimator_.beginFrame(reset);
      if (effectiveMotionConfig.mode != MotionEstimatorMode::Off) {
        pastMotion = motionEstimator_.estimate(
            effectiveMotionConfig, analysisLuma,
            sideBufferSynth_.previousLuma(), pastMotion,
            static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)));
        if (profileUploadEnv) {
          const auto &motionStats = motionEstimator_.stats();
          logInfo("PlaybackEngine: motion estimator mode={} seeds={} accepted={} "
                  "refined={} lowConfidence={} residual={:.5f} confidence={:.5f} "
                  "cpuMs={:.3f}",
                  effectiveMotionConfig.mode == MotionEstimatorMode::Codec
                      ? "codec" : "codec_refined",
                  motionStats.inputSeeds, motionStats.acceptedSeeds,
                  motionStats.refinedSeeds, motionStats.lowConfidenceSeeds,
                  motionStats.meanResidual, motionStats.meanConfidence,
                  motionStats.cpuMilliseconds);
        }
      }
      // Complete the persisted AutoCheap policy: use the refined codec field
      // when causal seeds exist, otherwise run the bounded luma matcher. The
      // Block and Zero settings are explicit payload policies; they must not
      // leave codec vectors from the initial adapter in the field.
      if (!explicitMotionSelector && !reset) {
        if (userMotionMode == MotionMode::Zero) {
          pastMotion.clear();
        } else if (userMotionMode == MotionMode::Block ||
                   (userMotionMode == MotionMode::AutoCheap &&
                    motionEstimator_.stats().inputSeeds == 0)) {
          pastMotion = sideBufferSynth_.estimateFallbackMotion(
              analysisLuma, static_cast<uint32_t>(std::max(0, df.width)),
              static_cast<uint32_t>(std::max(0, df.height)));
        }
      }
      // Controlled Phase 2 replay: consume only the validated dense-flow
      // tiles selected by the caller. A missing or malformed frame is a hard
      // opt-in replay miss, not permission to fall back to codec vectors,
      // because mixing correspondence sources would invalidate the A/B test.
      if (const char *denseMotionPath =
              std::getenv("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION")) {
        std::vector<MvEntry> denseMotion;
        // Dense replay sidecars are relative to the first scored output, not
        // to the decoder stream. Warmup frames must not anchor the sidecar or
        // consume its frame zero; doing so silently disabled every later
        // replay when a capture used DUMP_SEQUENCE_WARMUP.
        const bool replayFrameReady = df.frameIndex >= dumpSequenceWarmup;
        const uint64_t replayFrameIndex =
            replayFrameReady ? df.frameIndex - dumpSequenceWarmup : 0;
        if (replayFrameReady &&
            loadDenseMotionReplay(
                denseMotionPath, replayFrameIndex,
                seekGeneration_.load(std::memory_order_acquire), df.width,
                df.height, denseMotion)) {
          if (std::getenv("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION_HYBRID") &&
              !pastMotion.empty()) {
            // Keep the sparse causal field underneath the validated dense
            // tiles. The GPU expansion's deterministic last-writer rule lets
            // dense vectors override covered regions while sparse vectors
            // remain available in occluded/unvalidated regions.
            pastMotion.insert(pastMotion.end(), denseMotion.begin(),
                              denseMotion.end());
          } else {
            pastMotion = std::move(denseMotion);
          }
          if (profileUploadEnv) {
            logInfo("PlaybackEngine: dense motion replay frame={} relative={} vectors={}",
                    df.frameIndex, replayFrameIndex, pastMotion.size());
          }
        } else {
          pastMotion.clear();
          static bool warnedDenseReplayMiss = false;
          if (replayFrameReady && !warnedDenseReplayMiss) {
            logWarn("PlaybackEngine: dense motion replay has no valid relative frame {} "
                    "(decoder={} warmup={}) in {}",
                    replayFrameIndex, df.frameIndex, dumpSequenceWarmup,
                    denseMotionPath);
            warnedDenseReplayMiss = true;
          }
        }
      }
      // Diagnostic-only correspondence ablation: replace codec vectors with
      // the existing causal luma block matcher even when codec side data is
      // present. This isolates whether the decoder's sparse vectors are the
      // quality limiter; the normal path remains codec-first and unchanged.
      // Upstream: decoded frames plus the previous analysis luma. Downstream:
      // motion confidence, dense motion expansion, and FSR history sampling.
      if (!reset && std::getenv("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION")) {
        pastMotion = sideBufferSynth_.estimateFallbackMotion(
            analysisLuma, static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)));
        if (profileUploadEnv) {
          logInfo("PlaybackEngine: replacement block motion frame={} blocks={}",
                  df.frameIndex, pastMotion.size());
        }
      }
      // Optional cheap correction for codec vectors. The decoder vectors stay
      // the seed and the analysis-luma matcher only searches a one/two-pixel
      // neighborhood around each seed; the default path is unchanged.
      const bool legacyMotionRefinement =
          std::getenv("TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION") != nullptr;
      if (!pastMotion.empty() && !reset &&
          !std::getenv("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION") &&
          // An explicit standalone estimator owns the motion field. Keeping
          // the legacy best-findings refinement behind Off prevents the
          // `codec` selector from silently becoming codec-plus-refinement.
          motionEstimatorConfig.mode == MotionEstimatorMode::Off &&
          legacyMotionRefinement) {
        int refinementRadius = 1;
        if (const char *env = std::getenv("TFORGE_FSR4_MOTION_REFINE_RADIUS")) {
          char *end = nullptr;
          const long parsed = std::strtol(env, &end, 10);
          if (end != env && *end == '\0')
            refinementRadius = static_cast<int>(std::clamp(parsed, 0L, 2L));
        }
        float maxCorrectionPixels = 1.0f;
        if (const char *env =
                std::getenv("TFORGE_FSR4_MOTION_MAX_CORRECTION")) {
          char *end = nullptr;
          const float parsed = std::strtof(env, &end);
          if (end != env && *end == '\0' && std::isfinite(parsed))
            maxCorrectionPixels = std::clamp(parsed, 0.0f, 16.0f);
        }
        const auto refineStart = std::chrono::steady_clock::now();
        pastMotion = sideBufferSynth_.refineCodecMotion(
            analysisLuma, pastMotion, static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)), refinementRadius,
            maxCorrectionPixels);
        if (profileUploadEnv) {
          const double refineMs = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - refineStart).count();
          logInfo("PlaybackEngine: codec motion refinement frame={} blocks={} radius={} ms={:.3f}",
                  df.frameIndex, pastMotion.size(), refinementRadius, refineMs);
        }
      }
      // Some H.264/H.265 files expose no AV_FRAME_DATA_MOTION_VECTORS at
      // all. Keep the normal path unchanged, but let a controlled quality
      // capture use the analysis-luma block matcher as the documented
      // AutoCheap fallback for those clips. It is computed before update()
      // replaces SideBufferSynth's previous analysis frame.
      // Future-aligned jitter needs a usable current->previous field as well
      // as its future->current field. Some review clips carry no codec motion
      // side data, which otherwise makes SideBufferSynth report zero
      // confidence and reset FSR on every frame. Reuse the existing bounded
      // block matcher only for this explicit opt-in (or its already explicit
      // block-motion candidate); the normal causal path is untouched.
      const bool futureAlignedFallbackMotion =
          futureAlignedJitterEnv &&
          std::getenv("TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION") == nullptr;
      if (pastMotion.empty() && !reset &&
          (std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION") ||
           futureAlignedFallbackMotion)) {
        pastMotion = sideBufferSynth_.estimateFallbackMotion(
            analysisLuma, static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)));
        if (profileUploadEnv) {
          logInfo("PlaybackEngine: {} fallback motion frame={} blocks={}",
                  futureAlignedFallbackMotion ? "future-aligned" : "block",
                  df.frameIndex, pastMotion.size());
        }
      }
      // Optional bidirectional motion-consistency probe. The decode loop has
      // already buffered the next frame, so estimate its correspondence back
      // to the current frame and use only same-area agreement to adjust the
      // causal field. This is deliberately after the fallback selection and
      // before confidence/validity stages; the default remains causal.
      if (!pastMotion.empty() && !reset && hasPendingDecodedFrame &&
          std::getenv("TFORGE_FSR4_EXPERIMENTAL_BIDIRECTIONAL_MOTION")) {
        const LumaBuffer futureAnalysis = makeAnalysisLuma(pendingDecodedFrame);
        if (futureAnalysis.width == analysisLuma.width &&
            futureAnalysis.height == analysisLuma.height &&
            !futureAnalysis.data.empty()) {
          SideBufferSynth futureMotionSynth;
          futureMotionSynth.update(analysisLuma, 0.0f, false);
          const auto futureMotion = futureMotionSynth.estimateFallbackMotion(
              futureAnalysis, static_cast<uint32_t>(std::max(0, df.width)),
              static_cast<uint32_t>(std::max(0, df.height)));
          pastMotion = SideBufferSynth::fuseBidirectionalMotion(
              pastMotion, futureMotion);
          if (profileUploadEnv) {
            logInfo("PlaybackEngine: bidirectional motion frame={} past={} future={}",
                    df.frameIndex, pastMotion.size(), futureMotion.size());
          }
        }
      }
      // Future-frame evidence-only probe. The buffered next frame challenges
      // current-to-previous motion, but its pixels never become the current
      // color input and its opposite-direction vector is never averaged into
      // the causal vector. This tests the interpolation hypothesis without
      // displaying an interpolated frame or changing frame cadence.
      if (!pastMotion.empty() && !reset && hasPendingDecodedFrame &&
          std::getenv("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY")) {
        const LumaBuffer futureAnalysis = makeAnalysisLuma(pendingDecodedFrame);
        if (futureAnalysis.width == analysisLuma.width &&
            futureAnalysis.height == analysisLuma.height &&
            !futureAnalysis.data.empty()) {
          SideBufferSynth futureMotionSynth;
          futureMotionSynth.update(analysisLuma, 0.0f, false);
          const auto futureMotion = futureMotionSynth.estimateFallbackMotion(
              futureAnalysis, static_cast<uint32_t>(std::max(0, df.width)),
              static_cast<uint32_t>(std::max(0, df.height)));
          pastMotion = SideBufferSynth::gateMotionWithFutureEvidence(
              pastMotion, futureMotion);
          if (profileUploadEnv) {
            logInfo("PlaybackEngine: future evidence-only frame={} past={} future={}",
                    df.frameIndex, pastMotion.size(), futureMotion.size());
          }
        } else if (profileUploadEnv) {
          logWarn("PlaybackEngine: future evidence skipped frame={} current-analysis={}x{} future-analysis={}x{}",
                  df.frameIndex, analysisLuma.width, analysisLuma.height,
                  futureAnalysis.width, futureAnalysis.height);
        }
      } else if (profileUploadEnv &&
                 std::getenv("TFORGE_FSR4_EXPERIMENTAL_FUTURE_EVIDENCE_ONLY")) {
        logWarn("PlaybackEngine: future evidence unavailable frame={} past={} reset={} pending={}",
                df.frameIndex, pastMotion.size(), reset, hasPendingDecodedFrame);
      }
      // Optional Phase 3 history-confidence probe. Reject only vectors whose
      // local destination patch is not supported by the previous analysis
      // frame; uncovered pixels are then rejected by the existing shader
      // validity path. Upstream: codec or validated replay motion. Downstream:
      // motion upload, history confidence, and recurrent reprojection. The
      // normal path remains unchanged unless explicitly enabled.
      if (!pastMotion.empty() && !reset &&
          (bestFindingsTemporal ||
           std::getenv("TFORGE_FSR4_EXPERIMENTAL_VALIDATE_MOTION"))) {
        float maxPatchError = 0.08f;
        if (const char *value =
                std::getenv("TFORGE_FSR4_MOTION_VALIDATION_MAX_ERROR")) {
          char *end = nullptr;
          const float parsed = std::strtof(value, &end);
          if (end != value && *end == '\0' && std::isfinite(parsed))
            maxPatchError = std::clamp(parsed, 0.0f, 1.0f);
        }
        pastMotion = sideBufferSynth_.validateCodecMotion(
            analysisLuma, pastMotion,
            static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)), maxPatchError);
        if (profileUploadEnv) {
          logInfo("PlaybackEngine: validated motion frame={} vectors={} maxError={}",
                  df.frameIndex, pastMotion.size(), maxPatchError);
        }
      }
      // Optional continuous confidence map for Phase 3. This retains every
      // vector and annotates it from local luma agreement; the existing GPU
      // expansion carries the score to R8 validity, and the experimental
      // prepass turns it into a smooth history weight.
      if (!pastMotion.empty() && !reset &&
          (bestFindingsTemporal ||
           std::getenv("TFORGE_FSR4_EXPERIMENTAL_MOTION_CONFIDENCE_MAP"))) {
        float errorScale = 0.04f;
        if (const char *value = std::getenv("TFORGE_FSR4_MOTION_CONFIDENCE_SCALE")) {
          char *end = nullptr;
          const float parsed = std::strtof(value, &end);
          if (end != value && *end == '\0' && std::isfinite(parsed))
            errorScale = std::clamp(parsed, 0.001f, 1.0f);
        }
        pastMotion = sideBufferSynth_.scoreCodecMotion(
            analysisLuma, pastMotion,
            static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)), errorScale);
      }
      const float futureAnalysisConfidence =
          hasPendingDecodedFrame
              ? lookaheadConfidence(df, pendingDecodedFrame)
              : 1.0f;
      // Preserve the confidence that the real motion candidate would have
      // supplied before a benchmark ablation changes the vector payload. This
      // keeps the scene-cut detector, jitter phase, and temporal trust policy
      // identical between the real-motion and zero-motion arms; otherwise a
      // variable jitter sequence can make the A/B comparison non-causal.
      const float preAblationMotionConfidence =
          codecMotionConfidence(pastMotion, df.width, df.height) *
          futureAnalysisConfidence;
      // Benchmark-only Phase 1 motion ablation. The exact A–G matrix needs a
      // real codec-vector arm, a real zero-vector arm, and a separately
      // computed cheap block-matcher arm. Applying this after all normal
      // motion validation/refinement keeps the ablation boundary explicit:
      // `codec` preserves the ordinary path, `zero` removes correspondence,
      // and `block` replaces it with the causal luma matcher. The default is
      // unchanged when the variable is absent. Upstream: decoded-frame luma
      // and the normal pastMotion candidate. Downstream: confidence, side
      // buffers, history reprojection, and recurrent-state input.
      const char *motionAblation =
          std::getenv("TFORGE_FSR4_MOTION_ABLATION");
      const bool zeroMotionAblation =
          !reset && motionAblation && std::strcmp(motionAblation, "zero") == 0;
      if (motionAblation) {
        if (zeroMotionAblation) {
          pastMotion.clear();
        } else if (!reset && std::strcmp(motionAblation, "block") == 0) {
          pastMotion = sideBufferSynth_.estimateFallbackMotion(
              analysisLuma, static_cast<uint32_t>(std::max(0, df.width)),
              static_cast<uint32_t>(std::max(0, df.height)));
        }
      }
      // The sparse expansion stage resolves overlaps by retaining the last
      // stamped vector. When explicitly requested, make that deterministic
      // rule confidence-aware after every motion source/ablation has settled.
      // Jitter is intentionally untouched so this remains a motion-only arm.
      if (!pastMotion.empty() &&
          (bestFindingsTemporal ||
           std::getenv("TFORGE_FSR4_EXPERIMENTAL_CONFIDENCE_ORDERED_MOTION"))) {
        orderMotionByConfidence(pastMotion);
      }
      // Apply the UI/benchmark value on the decode thread immediately before
      // synthesizing side inputs. The setter is intentionally atomic because
      // it can be called from Qt's UI thread while this loop is running; the
      // synthesizer itself is owned and updated by this loop.
      sideBufferSynth_.setJitterStrength(
          jitterStrength_.load(std::memory_order_acquire));
      // Previous-jitter metadata is part of the published-frame transaction,
      // not merely scratch state for this dispatch. Snapshot it before the
      // candidate phase is prepared so a later upload, dispatch, or
      // presentation failure restores the exact prior history origin.
      // Upstream: the last published FSR frame. Downstream: this frame's
      // previous-jitter uniforms and the next frame's history alignment.
      const bool hasPreviousJitterBeforeFrame = hasPreviousJitter;
      const float previousJitterXBeforeFrame = previousJitterX;
      const float previousJitterYBeforeFrame = previousJitterY;
      const float sideMotionConfidence =
          zeroMotionAblation ? preAblationMotionConfidence
                             : codecMotionConfidence(pastMotion, df.width,
                                                     df.height) *
                                   futureAnalysisConfidence;
      const SideBufferInputs sideInputs = sideBufferSynth_.update(
          analysisLuma, ptsDeltaMs, reset, sideMotionConfidence);
      // update() advances a candidate jitter phase before this decode loop
      // knows whether the frame can reach FSR. Keep rollback armed across
      // every wait, abort, upload, and initialization exit. The existing
      // successful-dispatch commit below disarms it only after submission.
      // Upstream: SideBufferSynth::update(). Downstream: FSR color sampling
      // and metadata, which must never skip a phase for an unsubmitted frame.
      auto rollbackJitter = [&]() {
        sideBufferSynth_.rollbackJitter(sideInputs.reset || reset);
        // PTS analysis and jitter/history are one frame transaction. Restore
        // the timestamp origin if this frame never reaches a published FSR
        // output, so the next successful frame measures from the last
        // published frame rather than from an unpublished attempt.
        lastAnalysisPtsUs_ = analysisPtsBeforeFrame;
        // The reset request is consumed before dispatch so this frame can
        // carry it into the FSR constants. If the frame never publishes,
        // restore the request; a newly recreated history resource must not be
        // treated as valid merely because its first submission failed.
        if (requestedTemporalReset)
          fsrTemporalResetRequested_.store(true, std::memory_order_release);
        previousJitterX = previousJitterXBeforeFrame;
        previousJitterY = previousJitterYBeforeFrame;
        hasPreviousJitter = hasPreviousJitterBeforeFrame;
      };
      struct JitterPhaseGuard {
        decltype(rollbackJitter) &rollback;
        bool committed = false;
        ~JitterPhaseGuard() {
          if (!committed)
            rollback();
        }
        void markCommitted() { committed = true; }
      } jitterPhaseGuard{rollbackJitter};
      // Future-aligned history diagnostics need to distinguish a real
      // history comparison from a reset-only frame. Keep this trace behind
      // the existing upload profiler so normal playback has no extra logging
      // or image-path work. Upstream: detector output and filtered motion.
      // Downstream: the explicit reset bit, motion upload, and prepass history
      // read. This is evidence-only; it does not change either path.
      if (profileUploadEnv && futureAlignedJitterEnv) {
        logInfo("PlaybackEngine: future-aligned handoff frame={} forcedReset={} "
                "detectorReset={} motionConfidence={:.4f} motionBlocks={} "
                "lookahead={} pending={} jitter=({}, {})",
                df.frameIndex, reset, sideInputs.reset,
                sideInputs.motionConfidence, pastMotion.size(),
                futureAnalysisConfidence, hasPendingDecodedFrame,
                sideInputs.jitterX, sideInputs.jitterY);
      }
      // Never carry an analysis match across a detected cut. Codec motion is
      // subject to the same causal reset policy, so this keeps both sources
      // consistent when the side-buffer detector rejects the transition.
      if (sideInputs.reset)
        pastMotion.clear();
      lastReactive_.store(sideInputs.reactiveAverage, std::memory_order_release);
      lastMotionConf_.store(sideInputs.motionConfidence, std::memory_order_release);
      if (sideInputs.reset && !reset) {
        sceneCuts_.fetch_add(1, std::memory_order_relaxed);
        historyResets_.fetch_add(1, std::memory_order_relaxed);
      }

      // --- FSR4 real-frame upscaling path (Phase A) ---
      // Run on `df` BEFORE building the render frame, because the upload
      // needs the YUV planes which would otherwise be moved into rf.
      // This is an experimental RE-derived image, but it must be
      // presented so visual quality can be evaluated. Validation/proof
      // status remains separate from presentation policy.
      bool fsr4Upscaled = false;
      uint32_t fsr4OutW = 0, fsr4OutH = 0;
      DecodedVideoFrame *fsrFrame = &df;
      DecodedVideoFrame interpolatedFrame;
      // This is the correspondence used between displayed midpoint frames.
      // It is intentionally separate from `pastMotion`, which was derived
      // for the original decoded frame before midpoint synthesis.
      std::vector<MvEntry> interpolatedMotion;
      // A synthesized midpoint has no motion/history correspondence to the
      // original `df` yet. Until a midpoint-to-midpoint field exists, force
      // this diagnostic frame to start a fresh temporal sample and do not
      // pair its pixels with the original frame's codec vectors. Upstream:
      // the display-interpolation probe. Downstream: motion upload and the
      // FSR prepass reset bit. The normal decoded-frame path is unaffected.
      bool interpolatedFrameTemporalReset = false;
      // This mode intentionally makes future pixels the actual FSR input and
      // displayed result. It is a midpoint blend control, not a claim that
      // motion-compensated interpolation is solved. If the source cannot be
      // blended honestly, stay on the current frame and make the miss visible
      // in the log rather than silently changing the experimental comparison.
      bool motionCompensatedMidpoint = false;
      if (interpolationProbeEnv && hasPendingDecodedFrame) {
        std::vector<MvEntry> futureToCurrent;
        if (interpolationMotionPath) {
          // Dense sidecars index the future frame in the adjacent pair. The
          // first current frame therefore consumes sidecar frame 1 (future
          // frame 1 projected back to current frame 0). The loader's relative
          // base keeps later requests aligned with the capture window.
          loadDenseMotionReplay(
              interpolationMotionPath, df.frameIndex + 1,
              seekGeneration_.load(std::memory_order_acquire), df.width,
              df.height, futureToCurrent);
        } else {
          const LumaBuffer futureAnalysis =
              makeAnalysisLuma(pendingDecodedFrame);
          if (futureAnalysis.width == analysisLuma.width &&
              futureAnalysis.height == analysisLuma.height &&
              !futureAnalysis.data.empty()) {
            SideBufferSynth interpolationMotionSynth;
            interpolationMotionSynth.update(analysisLuma, 0.0f, false);
            futureToCurrent = interpolationMotionSynth.estimateFallbackMotion(
                futureAnalysis, static_cast<uint32_t>(std::max(0, df.width)),
                static_cast<uint32_t>(std::max(0, df.height)));
          }
        }
        if (!futureToCurrent.empty()) {
          motionCompensatedMidpoint = futureAlignedJitterEnv
              ? makeFutureAlignedFrame(df, pendingDecodedFrame,
                                       futureToCurrent, interpolatedFrame,
                                       futureAlignPhotometricThreshold)
              : makeMotionCompensatedMidpointFrame(
                    df, pendingDecodedFrame, futureToCurrent, interpolatedFrame);
          if (motionCompensatedMidpoint && !pastMotion.empty() &&
              !futureAlignedJitterEnv) {
            // A displayed midpoint contains evidence from both endpoints.
            // Average the causal current->previous field with the
            // future->current field so the next midpoint can reproject the
            // previous midpoint instead of inheriting either endpoint's
            // correspondence alone. This remains opt-in and uses the same
            // validated same-area fusion primitive as the motion probe.
            interpolatedMotion = SideBufferSynth::fuseBidirectionalMotion(
                pastMotion, futureToCurrent);
          }
        }
        if (!motionCompensatedMidpoint && !interpolationMotionPath)
          motionCompensatedMidpoint =
              makeMidpointFrame(df, pendingDecodedFrame, interpolatedFrame);
        if (interpolationMotionPath && !motionCompensatedMidpoint) {
          static bool loggedInterpolationMotionMiss = false;
          if (!loggedInterpolationMotionMiss) {
            logWarn("PlaybackEngine: dense interpolation motion has no valid "
                    "future->current sidecar for frame {}", df.frameIndex);
            loggedInterpolationMotionMiss = true;
          }
        }
      }
      if (interpolationProbeEnv && motionCompensatedMidpoint) {
        fsrFrame = &interpolatedFrame;
        // Use only a midpoint-to-midpoint field. If one could not be built,
        // clear the endpoint field and reset rather than feed false motion.
        if (futureAlignedJitterEnv && !pastMotion.empty()) {
          // The future sample has already been warped onto the current
          // coordinate system, so the existing causal field remains the
          // correct field for current-frame history.
          interpolatedFrame.motionVectors = pastMotion;
        } else if (!interpolatedMotion.empty()) {
          pastMotion = std::move(interpolatedMotion);
          interpolatedFrame.motionVectors = pastMotion;
        } else {
          pastMotion.clear();
          interpolatedFrame.motionVectors.clear();
          interpolatedFrameTemporalReset = true;
        }
        if (interpolatedJitterEnv || futureAlignedJitterEnv) {
          // This is a temporal sample replacement, not frame interpolation
          // for presentation. Keep the original decode timeline so the
          // candidate is scored against the same displayed-frame reference.
          interpolatedFrame.ptsUs = df.ptsUs;
          interpolatedFrame.durationUs = df.durationUs;
        }
        static bool loggedInterpolatedProbe = false;
        if (!loggedInterpolatedProbe) {
          logInfo("PlaybackEngine: motion-compensated interpolated-display "
                  "probe active; midpoint decoded pixels are sent through FSR "
                  "and published at midpoint PTS");
          loggedInterpolatedProbe = true;
        }
      } else if (interpolationProbeEnv && hasPendingDecodedFrame) {
        static bool loggedInterpolatedMiss = false;
        if (!loggedInterpolatedMiss) {
          logWarn("PlaybackEngine: interpolated-display probe skipped; "
                  "adjacent frames are not compatible software 8-bit frames");
          loggedInterpolatedMiss = true;
        }
      }
      if (fsr4Enabled_.load(std::memory_order_acquire) &&
          vkDevice_ != VK_NULL_HANDLE &&
          !fsrAbortRequested_.load(std::memory_order_acquire)) {
        float selectedScale = fsrScale_.load(std::memory_order_acquire);
        if (const char *env = std::getenv("TFORGE_FSR4_FORCE_SCALE")) {
          char *end = nullptr;
          const float forced = std::strtof(env, &end);
          if (end != env && std::isfinite(forced) && forced >= 1.0f)
            selectedScale = forced;
        }
        const bool forcedViewport =
            std::getenv("TFORGE_FSR4_FORCE_VIEWPORT") != nullptr;
        const FsrJitterPair jitterPair = computeFsrJitterPair(
            static_cast<uint32_t>(std::max(0, df.width)),
            static_cast<uint32_t>(std::max(0, df.height)), selectedScale,
            forcedViewport,
            fsrViewportW_.load(std::memory_order_acquire),
            fsrViewportH_.load(std::memory_order_acquire),
            fsrTargetViewportW_.load(std::memory_order_acquire),
            fsrTargetViewportH_.load(std::memory_order_acquire));
        const uint32_t displayW = jitterPair.displayW;
        const uint32_t displayH = jitterPair.displayH;
        const bool nativePassthrough = jitterPair.nativePassthrough;
        const bool preEasu = jitterPair.preEasu;
        const uint32_t fsrModelW = jitterPair.modelW;
        const uint32_t fsrModelH = jitterPair.modelH;
        const DecodedVideoFrame &fsrDf = *fsrFrame;
        std::vector<MvEntry> temporalMotion;
        if (!nativePassthrough) {
          temporalMotion = scaleMotionCoverageToModel(
            pastMotion, fsrDf.width, fsrDf.height, fsrModelW, fsrModelH);
          if (dumpMotionSeedsEnv) {
            size_t nonZero = 0;
            for (const MvEntry &seed : temporalMotion) {
              if (seed.mvX != 0.0f || seed.mvY != 0.0f)
                ++nonZero;
            }
            logInfo("PlaybackEngine: pre-upload motion seeds frame={} "
                    "source={}x{} model={}x{} total={} nonZero={}",
                    df.frameIndex, fsrDf.width, fsrDf.height, fsrModelW,
                    fsrModelH, temporalMotion.size(), nonZero);
            size_t printed = 0;
            for (const MvEntry &seed : temporalMotion) {
              if ((seed.mvX == 0.0f && seed.mvY == 0.0f) || printed >= 64)
                continue;
              logInfo("PlaybackEngine: seed frame={} dst=({}, {}) size={}x{} "
                      "mv=({}, {}) confidence={} source={}",
                      df.frameIndex, seed.dstX, seed.dstY, seed.w, seed.h,
                      seed.mvX, seed.mvY, seed.confidence, seed.source);
              ++printed;
            }
          }
        }
        // FSR presents from one shared Vulkan image. Do not let the decode
        // thread overwrite that image while the previous frame is still
        // queued for presentation; otherwise the PTS and pixels can diverge.
        if (fsr4FrameReady_.load(std::memory_order_acquire) &&
            !headlessBenchmarkEnv) {
          std::unique_lock frameLock(frameMutex_);
          frameCv_.wait(frameLock, [&] {
            return !running_.load() || seekPending_.load() ||
                   queuedFrames_.load(std::memory_order_acquire) == 0;
          });
          if (!running_.load() || seekPending_.load())
            continue;
        }
        // The dispatch block holds fsrDispatchMutex_ for its whole duration,
        // including the lazy init/realloc. teardownFsr4Path (called from the
        // UI thread on preset/backend/file changes) blocks on this mutex and
        // then waits the GPU queue idle, so the Vulkan resources we touch
        // here cannot be freed under us and we cannot race a pointer read.
        std::unique_lock dispatchLock(fsrDispatchMutex_);
        if (fsrAbortRequested_.load(std::memory_order_acquire)) {
          // Teardown raced us. Bail before touching the harness/uploader;
          // the frame is emitted as a raw decoded frame instead.
          dispatchLock.unlock();
        } else {
          // Lazy-init / realloc on first frame or source-dim change. Done
          // under the lock so the sourceW/sourceH reads cannot race a
          // concurrent teardownFsr4Path.
          GpuImageUploader *configuredInput =
              fsr4IntermediateUploaders_.empty()
                  ? fsr4Uploader_.get()
                  : fsr4IntermediateUploaders_.front().get();
          if (!fsr4Ready_.load(std::memory_order_acquire) ||
              (configuredInput &&
               (configuredInput->sourceW() !=
                    static_cast<uint32_t>(fsrDf.width) ||
                configuredInput->sourceH() !=
                    static_cast<uint32_t>(fsrDf.height) ||
                configuredInput->modelW() != fsrModelW ||
                configuredInput->modelH() != fsrModelH))) {
            if (!initFsr4Path(fsrDf.width, fsrDf.height,
                              static_cast<int>(fsrModelW),
                              static_cast<int>(fsrModelH))) {
              fsr4Enabled_.store(
                  false,
                  std::memory_order_release); // init failed — stop retrying
              // The scene graph presents Vulkan images, not raw DRM frames.
              // If the neural path cannot initialize (for example, its weight
              // blob is unavailable), immediately fall back to the uploader's
              // EASU path so playback remains displayable instead of queuing
              // an unpresentable hardware frame and showing black.
              easuOnlyMode_.store(true, std::memory_order_release);
              logWarn("PlaybackEngine: FSR4 unavailable; using EASU-only "
                      "display fallback");
            }
          }
        }
        if (dispatchLock.owns_lock() &&
            !fsrAbortRequested_.load(std::memory_order_acquire) &&
            fsr4Ready_.load(std::memory_order_acquire) && fsr4Uploader_ &&
            fsr4Harness_) {
          const bool singlePass = fsr4PassSizes_.size() == 1;
          // The two overlap slots own separate history and recurrent images.
          // Alternating them would make a temporal dispatch read state from
          // the wrong frame, so keep one causal state chain whenever either
          // persistent temporal resource is enabled. Stateless playback can
          // still use the overlap optimization below. Synthetic jitter is
          // safe in that stateless case because there is no persistent
          // temporal state commit to serialize.
          const bool temporalStateEnabled =
              std::getenv("TFORGE_FSR4_ENABLE_COLOR_HISTORY") != nullptr ||
              std::getenv("TFORGE_FSR4_ENABLE_RECURRENT") != nullptr ||
              integratedHistoryConfidenceProfile;
          // Synthetic jitter is a transaction with the published frame: its
          // phase must advance only after the matching color sample and FSR
          // metadata have completed. The in-flight path returns before that
          // completion, so allowing it here would consume a phase for a frame
          // that may later fail or be dropped. Upstream: jitter mode and the
          // selected temporal input path. Downstream: SideBufferSynth's phase
          // and the next frame's FSR jitter offset.
          const bool syntheticJitterEnabled =
              syntheticJitterApplied &&
              !(jitterModeEnv && std::strcmp(jitterModeEnv, "off") == 0) &&
              !nativePassthrough && !prepassJitterOrdering;
          const bool asyncSlots =
              !bestFindingsTemporal && singlePass && !temporalStateEnabled &&
              !syntheticJitterEnabled &&
              !nativePassthrough &&
              !preEasu && fsr4InFlightUploader_ &&
              fsr4InFlightHarness_ &&
              // In-flight submission is an explicit throughput probe until
              // its fence/presentation result can own the same luma, jitter,
              // and frame-continuity commit boundary as the blocking path.
              std::getenv("TFORGE_FSR4_ENABLE_INFLIGHT") != nullptr &&
              std::getenv("TFORGE_FSR4_DISABLE_INFLIGHT") == nullptr;
          GpuImageUploader *firstUploader = nullptr;
          Fsr4DispatchHarness *firstHarness = nullptr;
          double retiredGpuWaitCpuMs = 0.0;
          double currentDispatchWaitCpuMs = 0.0;
          if (asyncSlots) {
            firstUploader = fsr4NextDispatchSlot_ == 0
                                 ? fsr4Uploader_.get()
                                 : fsr4InFlightUploader_.get();
            firstHarness = fsr4NextDispatchSlot_ == 0
                               ? fsr4Harness_.get()
                               : fsr4InFlightHarness_.get();

            // Retire this slot before reusing it. The other slot may still be
            // executing; its history is safe to consume because the next
            // dispatch is ordered on the same Vulkan queue and records an
            // explicit image barrier for the prior shader write.
            if (firstHarness->frameInFlight()) {
              const auto completed = firstHarness->waitForFrame();
              retiredGpuWaitCpuMs = completed.cpuWaitMs;
              firstUploader->completeDeferredFrameUploads();
              if (completed.ok) {
                if (!firstUploader->dispatchPresentationScaler(displayW,
                                                                 displayH)) {
                  logWarn("PlaybackEngine: in-flight presentation scaler "
                          "failed");
                } else {
                  // Commit temporal state only after the completed frame has
                  // also produced a valid presentation image. Otherwise a
                  // presentation failure would make an unpublished frame the
                  // history source for the next dispatch.
                  firstUploader->advanceHistory();
                  fsr4PublishedUploader_.store(firstUploader,
                                                std::memory_order_release);
                  fsr4FrameReady_.store(true, std::memory_order_release);
                  lastFsr4DispatchMs_.store(completed.dispatchMs,
                                            std::memory_order_release);
                  lastFsr4GpuMs_.store(completed.gpuMs,
                                       std::memory_order_release);
                  emit fsr4StatusChanged();
                }
              } else {
                logWarn("PlaybackEngine: in-flight FSR4 frame failed: {}",
                        completed.failReason);
              }
            }
          } else {
            firstUploader = fsr4IntermediateUploaders_.empty()
                                ? fsr4Uploader_.get()
                                : fsr4IntermediateUploaders_.front().get();
            firstHarness = fsr4IntermediateHarnesses_.empty()
                               ? fsr4Harness_.get()
                               : fsr4IntermediateHarnesses_.front().get();
          }
          const auto fsrPipelineStart = std::chrono::steady_clock::now();
          const float desiredSharpness =
              sharpness_.load(std::memory_order_acquire);
          if (desiredSharpness != fsr4AppliedSharpness_) {
            firstUploader->setSharpness(desiredSharpness);
            fsr4AppliedSharpness_ = desiredSharpness;
          }
          const bool desiredCompareEnabled =
              compareEnabled_.load(std::memory_order_acquire);
          if (desiredCompareEnabled != fsr4AppliedCompareEnabled_) {
            firstUploader->setCompareEnabled(desiredCompareEnabled);
            fsr4AppliedCompareEnabled_ = desiredCompareEnabled;
          }
          // Upload all per-frame inputs in one transfer submission.
          // Each uploader operation records into this command buffer;
          // the single fence wait happens after the batch.
          // Steady-state color conversion and GPU motion expansion share one
          // submission. Reset-time compatibility textures still use their
          // synchronous initialization path because they reuse staging memory.
          double colorUploadMs = 0.0;
          double motionUploadMs = 0.0;
          double neutralUploadMs = 0.0;
          double uploadFinalizeMs = 0.0;
          VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
          // The uploader samples the decoded source image, while FSR reads
          // the model-sized color image. Keep the same physical subpixel
          // displacement by converting the decoded-pixel sample into model
          // pixels only at the FSR metadata boundary. Upstream: the jitter
          // policy is authored in decoded pixels. Downstream: prepass and
          // history reprojection interpret FrameDispatchInput in model
          // pixels. Without this conversion reduced 720p paths report a
          // displacement in the wrong coordinate space.
          const float jitterToModelX = fsrDf.width > 0
              ? static_cast<float>(fsrModelW) /
                    static_cast<float>(fsrDf.width)
              : 1.0f;
          const float jitterToModelY = fsrDf.height > 0
              ? static_cast<float>(fsrModelH) /
                    static_cast<float>(fsrDf.height)
              : 1.0f;
          const float modelJitterX = sideInputs.jitterX * jitterToModelX * jitterSign;
          const float modelJitterY = sideInputs.jitterY * jitterToModelY * jitterSign;
          const float modelPreviousJitterX = hasPreviousJitter
              ? previousJitterX
              : sideInputs.jitterX * jitterToModelX * jitterSign;
          const float modelPreviousJitterY = hasPreviousJitter
              ? previousJitterY
              : sideInputs.jitterY * jitterToModelY * jitterSign;
          const bool initializeNeutral =
              sideInputs.reset || !fsr4FrameReady_.load(std::memory_order_acquire);
          // The EASU pre-neural candidate needs ordered submissions between
          // color conversion, EASU, and the RGB10 model-image handoff. Keep
          // the ordinary path batched, while the opt-in path uses the
          // uploader's existing synchronous command submission boundaries.
          bool uploadOk =
              firstUploader->beginFrameUploads(!initializeNeutral && !preEasu);
          if (uploadOk) {
            // The motion estimator consumed unjittered analysis luma above.
            // Only the color construction receives this matching synthetic
            // sampling offset; uploadMotion() remains unjittered by contract.
            firstUploader->setInputJitter(
                syntheticJitterApplied && !nativePassthrough &&
                        !prepassJitterOrdering
                    ? sideInputs.jitterX * jitterSign * jitterSampleSign
                    : 0.0f,
                syntheticJitterApplied && !nativePassthrough &&
                        !prepassJitterOrdering
                    ? sideInputs.jitterY * jitterSign * jitterSampleSign
                    : 0.0f,
                syntheticJitterApplied && !nativePassthrough &&
                    !prepassJitterOrdering &&
                    (sideInputs.jitterX != 0.0f || sideInputs.jitterY != 0.0f));
            const auto colorUploadStart = std::chrono::steady_clock::now();
            uploadOk = firstUploader->uploadColor(fsrDf);
            colorUploadMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - colorUploadStart)
                    .count();
            // 2. Upload side-buffer textures (real modes).
            if (fsrDf.planes > 0) {
              // Luma is only needed for the reset-time side buffers.
            }
            if (!nativePassthrough) {
              const auto motionUploadStart = std::chrono::steady_clock::now();
              uploadOk &= firstUploader->uploadMotion(
                  temporalMotion, effectiveMotionConfig.edgeAwareUpscale);
              motionUploadMs =
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - motionUploadStart)
                      .count();
            }

            // These are compatibility inputs inherited from the game
            // model, not changing video-frame data. Rebuilding Sobel
            // depth and a 3x3 reactive field over every source pixel
            // stalled the decode thread and injected frame-to-frame
            // noise into the reconstruction. Initialize stable neutral
            // values when temporal state resets; color and codec motion
            // are the only per-frame uploads.
            if (initializeNeutral && !nativePassthrough) {
              const auto neutralUploadStart = std::chrono::steady_clock::now();
              SideBufferSource sbs;
              sbs.motionVectors = &temporalMotion;
              if (fsrDf.planes > 0) {
                sbs.luma = fsrDf.plane[0].data();
                sbs.lumaWidth = fsrDf.width;
                sbs.lumaHeight = fsrDf.height;
                sbs.lumaLinesize = fsrDf.linesize[0];
              }
              sbs.reactiveAverage = sideInputs.reactiveAverage;
              sbs.exposureScalar = 1.0f;
              uploadOk &= firstUploader->uploadDepthFlat();
              uploadOk &=
                  firstUploader->uploadReactive(sbs, /*aggressive=*/false);
              uploadOk &= firstUploader->clearTcMask();
              uploadOk &= firstUploader->uploadExposure(1.0f);
              neutralUploadMs =
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - neutralUploadStart)
                      .count();
            }
            if (uploadOk && preEasu) {
              // EASU consumes the converted native display image. Its RGBA8
              // result is then converted into the RGB10/A2 model image that
              // FSR4 actually reads. This is the complete candidate path,
              // not a display-only preview: native -> EASU 2x -> FSR4.
              uploadOk &= firstUploader->dispatchEasu();
              if (uploadOk && dumpModelInputEnv) {
                std::vector<uint8_t> easuReadback;
                uint32_t easuDumpW = 0, easuDumpH = 0;
                if (firstUploader->readbackEasu(easuReadback, easuDumpW,
                                                easuDumpH)) {
                  const auto [minIt, maxIt] = std::minmax_element(
                      easuReadback.begin(), easuReadback.end());
                  logInfo("PlaybackEngine: pre-neural EASU image frame={} "
                          "{}x{} byte-min={} byte-max={} fingerprint=0x{:016x}",
                          fsrFrame->frameIndex, easuDumpW, easuDumpH,
                          static_cast<unsigned>(*minIt),
                          static_cast<unsigned>(*maxIt),
                          diagnosticFingerprint(easuReadback));
                } else {
                  logWarn("PlaybackEngine: pre-neural EASU image readback "
                          "failed");
                }
              }
              uploadOk &= firstHarness->downscaleRgb10(
                  firstUploader->easuColorImage(),
                  firstUploader->easuColorView(), firstUploader->easuW(),
                  firstUploader->easuH(), firstUploader->colorImage(),
                  firstUploader->colorView(), firstUploader->modelW(),
                  firstUploader->modelH());
              if (uploadOk && dumpModelInputEnv &&
                  !fsr4DumpedModelInput_) {
                std::vector<uint8_t> modelReadback;
                uint32_t modelDumpW = 0, modelDumpH = 0;
                if (firstUploader->readbackModelColor(
                        modelReadback, modelDumpW, modelDumpH)) {
                  const auto *words = reinterpret_cast<const uint32_t *>(
                      modelReadback.data());
                  uint32_t minWord = UINT32_MAX;
                  uint32_t maxWord = 0;
                  const size_t count = static_cast<size_t>(modelDumpW) *
                                       modelDumpH;
                  for (size_t i = 0; i < count; ++i) {
                    minWord = std::min(minWord, words[i]);
                    maxWord = std::max(maxWord, words[i]);
                  }
                  logInfo("PlaybackEngine: pre-neural model input frame={} "
                          "{}x{} packed-min=0x{:08x} packed-max=0x{:08x} "
                          "fingerprint=0x{:016x}",
                          fsrFrame->frameIndex, modelDumpW, modelDumpH,
                          minWord, maxWord, diagnosticFingerprint(modelReadback));
                } else {
                  logWarn("PlaybackEngine: pre-neural model input readback "
                          "failed");
                }
                fsr4DumpedModelInput_ = true;
              }
              if (!uploadOk)
                logWarn("PlaybackEngine: EASU pre-neural handoff failed");
            }
            const auto uploadFinalizeStart = std::chrono::steady_clock::now();
            // The decoded color and motion uploads belong to firstUploader.
            // This is an intermediate uploader for chained passes, so ending
            // the final output uploader's batch leaves the real batch open
            // and makes the next frame fail beginFrameUploads().
            uploadOk &= firstUploader->endFrameUploads(nativePassthrough ? nullptr
                                                                            : &uploadCommandBuffer);
            uploadFinalizeMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - uploadFinalizeStart)
                    .count();
          }
          if (!uploadOk && firstUploader) {
            firstUploader->endFrameUploads();
          }

          if (!uploadOk) {
            logWarn("PlaybackEngine: FSR4 input upload failed");
          } else {
            if (nativePassthrough) {
              // endFrameUploads(nullptr) above has already submitted and
              // waited for the color conversion. Publish the raw image so
              // both display and diagnostics see the exact native frame.
              fsr4PublishedUploader_.store(firstUploader,
                                            std::memory_order_release);
              fsr4NativePassthrough_.store(true, std::memory_order_release);
              fsr4FrameReady_.store(true, std::memory_order_release);
              fsr4Upscaled = true;
              fsr4OutW = firstUploader->sourceW();
              fsr4OutH = firstUploader->sourceH();
              lastFsr4DispatchMs_.store(0.0, std::memory_order_release);
              lastFsr4GpuMs_.store(0.0, std::memory_order_release);
              emit fsr4StatusChanged();

              // Quality capture uses the same native image for output,
              // presentation, and raw controls. Readback is opt-in and only
              // occurs for those diagnostic environment switches.
              const auto dumpNative = [&](const char *path,
                                          const char *label) {
                std::vector<uint8_t> nativeReadback;
                uint32_t dumpW = 0, dumpH = 0;
                if (!firstUploader->readbackRaw(nativeReadback, dumpW, dumpH)) {
                  logWarn("PlaybackEngine: native {} readback failed", label);
                  return;
                }
                std::ofstream dump(path, std::ios::binary | std::ios::trunc);
                if (!dump)
                  return;
                dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                     ++i)
                  dump.write(reinterpret_cast<const char *>(
                                 nativeReadback.data() + i * 4),
                             3);
                logInfo("PlaybackEngine: dumped native {} frame={} {}x{} to {}",
                        label, fsrFrame->frameIndex, dumpW, dumpH, path);
              };
              const bool nativeDumpFrameReady =
                  fsrFrame->frameIndex >= dumpOutputFrame;
              if (nativeDumpFrameReady && dumpOutputEnv &&
                  !fsr4DumpedOutput_) {
                dumpNative(dumpOutputPath, "FSR4 output");
                fsr4DumpedOutput_ = true;
              }
              if (nativeDumpFrameReady && dumpPresentationEnv &&
                  !fsr4DumpedPresentation_) {
                dumpNative(dumpPresentationPath, "presentation");
                fsr4DumpedPresentation_ = true;
              }
              if (nativeDumpFrameReady && dumpRawEnv && !fsr4DumpedRaw_) {
                dumpNative(dumpRawPath, "raw");
                fsr4DumpedRaw_ = true;
              }
            } else {

            FrameDispatchInput in;
            in.prefixCommandBuffer = uploadCommandBuffer;
            in.colorView = firstUploader->colorView();
            in.colorImage = firstUploader->colorImage();
            // The raw decoded presentation image is the stable default source
            // for postpass composition. The explicit pre-EASU diagnostic then
            // replaces that source only after its opt-in dispatch has
            // completed, keeping the ordinary temporal path on the raw image.
            in.sourceDisplayView = firstUploader->rawPresentationView();
            in.sourceDisplayImage = firstUploader->rawPresentationImage();
            if (preEasu) {
              in.sourceDisplayView = firstUploader->easuColorView();
              in.sourceDisplayImage = firstUploader->easuColorImage();
            }
            in.motionView = firstUploader->motionView();
            in.motionValidityView = firstUploader->motionValidityView();
            in.motionImage = firstUploader->motionImage();
            in.motionValidityImage = firstUploader->motionValidityImage();
            in.depthView = firstUploader->depthView();
            in.reactiveView = firstUploader->reactiveView();
            in.tcMaskView = firstUploader->tcMaskView();
            in.exposureView = firstUploader->exposureView();
            in.outputView = firstUploader->outputView();
            // In the two-slot path, temporal history comes from the prior
            // submitted slot while the current slot owns the write images.
            // Reset frames intentionally keep the current slot's read views;
            // postpass ignores them when reset is asserted.
            GpuImageUploader *temporalSource =
                asyncSlots && fsr4LastSubmittedUploader_ &&
                        fsr4FrameReady_.load(std::memory_order_acquire)
                    ? fsr4LastSubmittedUploader_
                    : firstUploader;
            in.historyReadView = temporalSource->historyReadView();
            in.historyWriteView = firstUploader->historyWriteView();
            in.reprojectedColorView = firstUploader->reprojectedColorView();
            in.recurrentReadView = temporalSource->recurrentReadView();
            in.recurrentWriteView = firstUploader->recurrentWriteView();
            in.outputImage = firstUploader->outputImage();
            in.historyReadImage = temporalSource->historyReadImage();
            in.historyWriteImage = firstUploader->historyWriteImage();
            in.reprojectedColorImage = firstUploader->reprojectedColorImage();
            in.recurrentReadImage = temporalSource->recurrentReadImage();
            in.recurrentWriteImage = firstUploader->recurrentWriteImage();
            // Independent chained passes do not share motion/history at the
            // same resolution. Reset the first pass too, otherwise its
            // temporal reprojection shifts the source seen by later passes.
            const bool multipass = fsr4PassSizes_.size() > 1;
            in.reset = multipass || sideInputs.reset || forceResetEnv ||
                       interpolatedFrameTemporalReset;
            // The midpoint itself is the real temporal phase sample in this
            // candidate. Passing Halton offsets as well would combine two
            // unrelated phase mechanisms and make this an interpolation-plus
            // jitter experiment instead of a jitter replacement.
            const bool replaceSyntheticJitter =
                interpolatedJitterEnv || futureAlignedJitterEnv;
            in.jitterX = replaceSyntheticJitter ? 0.0f : modelJitterX;
            in.jitterY = replaceSyntheticJitter ? 0.0f : modelJitterY;
            in.previousJitterX = replaceSyntheticJitter
                                     ? 0.0f
                                     : modelPreviousJitterX;
            in.previousJitterY = replaceSyntheticJitter
                                     ? 0.0f
                                     : modelPreviousJitterY;
            in.frameTimeMs = ptsDeltaMs > 0.0f ? ptsDeltaMs : 16.6667f;
            in.historyConfidence = sideInputs.motionConfidence;
            in.reactiveAverage = sideInputs.reactiveAverage;
            in.hdr = fsrDf.colorTransfer == AVCOL_TRC_SMPTE2084 ||
                     fsrDf.colorTransfer == AVCOL_TRC_ARIB_STD_B67;
            in.transfer = fsrDf.colorTransfer == AVCOL_TRC_SMPTE2084
                              ? 1u
                              : fsrDf.colorTransfer == AVCOL_TRC_ARIB_STD_B67
                                    ? 2u
                                    : 0u;

            const bool runAsync = asyncSlots && !dumpOutputEnv &&
                                  !dumpPresentationEnv &&
                                  !dumpSequenceLimit && !dumpDecoderEnv &&
                                  !dumpRawEnv && !dumpModelInputEnv;
            // Stateful single-pass playback can record the presentation scaler
            // into the FSR command buffer. Stateless in-flight frames and
            // chained passes keep their existing publication path because
            // their completion/ownership boundaries are different.
            bool presentationFused = false;
            if (!runAsync && !nativePassthrough &&
                std::getenv("TFORGE_FSR4_DISABLE_FUSED_PRESENTATION") == nullptr &&
                fsr4PassSizes_.size() == 1 && firstUploader == fsr4Uploader_.get()) {
              if (firstUploader->preparePresentationScaler(displayW, displayH)) {
                in.appendPresentation =
                    [firstUploader, displayW, displayH](VkCommandBuffer command) {
                      return firstUploader->recordPresentationScaler(
                          command, displayW, displayH);
                    };
                presentationFused = true;
              }
            }
            auto dr = runAsync ? firstHarness->dispatchFrameAsync(in)
                               : firstHarness->dispatchFrame(in);
            currentDispatchWaitCpuMs += dr.cpuWaitMs;
            double chainDispatchMs = dr.dispatchMs;
            double chainGpuMs = dr.gpuMs;
            if (!runAsync) {
              firstUploader->completeDeferredFrameUploads();
            } else {
              // Keep the prefix command buffer and current slot resources
              // owned by the pending fence. The next decode iteration uses
              // the other slot; this slot is retired before it is reused.
              fsr4LastSubmittedUploader_ = firstUploader;
              fsr4NextDispatchSlot_ ^= 1u;
              fsr4Upscaled = true;
              fsr4OutW = firstUploader->outputW();
              fsr4OutH = firstUploader->outputH();
            }
            // A normal temporal capture can request the same model-input
            // fingerprint without enabling EASU. Upstream: the completed
            // color-upload command and FSR dispatch. Downstream: paired A/B
            // provenance, which can now prove whether an opt-in prefilter
            // changed the actual model input instead of only producing a log.
            if (dumpModelInputEnv && !preEasu && !fsr4DumpedModelInput_ &&
                !runAsync && fsrFrame->frameIndex >= dumpModelInputFrame) {
              if (*dumpStageDirectory)
                std::filesystem::create_directories(dumpStageDirectory);
              std::vector<uint8_t> sourceModelReadback;
              uint32_t sourceModelW = 0, sourceModelH = 0;
              if (firstUploader->readbackSourceModel(
                      sourceModelReadback, sourceModelW, sourceModelH)) {
                logInfo("PlaybackEngine: sourceModel stage frame={} {}x{} "
                        "fingerprint=0x{:016x}", fsrFrame->frameIndex,
                        sourceModelW, sourceModelH,
                        diagnosticFingerprint(sourceModelReadback));
                if (*dumpStageDirectory)
                  dumpPackedRgb10Ppm(
                      std::filesystem::path(dumpStageDirectory) /
                          "stage-A-sourceModel.ppm",
                      sourceModelReadback, sourceModelW, sourceModelH);
              }
              std::vector<uint8_t> modelReadback;
              uint32_t modelDumpW = 0, modelDumpH = 0;
              if (firstUploader->readbackModelColor(
                      modelReadback, modelDumpW, modelDumpH)) {
                logInfo("PlaybackEngine: model input fingerprint "
                        "(ordinary path) frame={} {}x{} "
                        "fingerprint=0x{:016x}",
                        fsrFrame->frameIndex, modelDumpW, modelDumpH,
                        diagnosticFingerprint(modelReadback));
                if (*dumpStageDirectory)
                  dumpPackedRgb10Ppm(
                      std::filesystem::path(dumpStageDirectory) /
                          "stage-B-color.ppm",
                      modelReadback, modelDumpW, modelDumpH);
              } else {
                logWarn("PlaybackEngine: ordinary model input readback "
                        "failed");
              }
              fsr4DumpedModelInput_ = true;
            }
            // Feed each later pass from the preceding pass's completed output.
            const size_t passCount = fsr4PassSizes_.size();
            auto uploaderAt = [&](size_t index) -> GpuImageUploader * {
              return index + 1 == passCount
                         ? fsr4Uploader_.get()
                         : fsr4IntermediateUploaders_[index].get();
            };
            auto harnessAt = [&](size_t index) -> Fsr4DispatchHarness * {
              return index + 1 == passCount
                         ? fsr4Harness_.get()
                         : fsr4IntermediateHarnesses_[index].get();
            };
            for (size_t pass = 1; dr.ok && pass < passCount; ++pass) {
              GpuImageUploader *previous = uploaderAt(pass - 1);
              GpuImageUploader *current = uploaderAt(pass);
              Fsr4DispatchHarness *previousHarness = harnessAt(pass - 1);
              const Size2D &previousSize = fsr4PassSizes_[pass - 1];
              // A chained pass must never read the decoded input again. Feed
              // it only the completed, upscaled output of the prior pass,
              // reduced to this pass's source dimensions on the GPU.
              const VkImage previousUpscaledOutput = previous->outputImage();
              const VkImageView previousUpscaledView = previous->outputView();
              if (!previousHarness->downscaleRgb10(
                      previousUpscaledOutput, previousUpscaledView,
                      previousSize.width, previousSize.height,
                      current->colorImage(),
                      current->colorView(), current->sourceW(),
                      current->sourceH())) {
                logWarn("PlaybackEngine: FSR4 chain downscale failed at pass {}",
                        pass);
                dr.ok = false;
                break;
              }
              if (initializeNeutral) {
                SideBufferSource neutral;
                neutral.reactiveAverage = 0.0f;
                neutral.exposureScalar = 1.0f;
                if (!current->uploadDepthFlat() ||
                    !current->uploadReactive(neutral, false) ||
                    !current->clearTcMask() || !current->uploadExposure(1.0f)) {
                  logWarn("PlaybackEngine: FSR4 chain neutral input setup failed at pass {}",
                          pass);
                  dr.ok = false;
                  break;
                }
              }
              FrameDispatchInput chained{};
              // This is the GPU-generated downscaled copy above, not
              // firstUploader->colorView().
              chained.colorView = current->colorView();
              chained.colorImage = current->colorImage();
              // The preceding pass output is display RGB and is the actual
              // color source for this chained pass. Keep it separate from the
              // model-space copy produced by downscaleRgb10().
              chained.sourceDisplayView = previousUpscaledView;
              chained.sourceDisplayImage = previousUpscaledOutput;
              // Auxiliary metadata remains in the original decoded-frame
              // domain; only the color frame changes between passes.
              chained.motionView = firstUploader->motionView();
              chained.motionValidityView = firstUploader->motionValidityView();
              chained.motionImage = firstUploader->motionImage();
              chained.motionValidityImage = firstUploader->motionValidityImage();
              chained.depthView = firstUploader->depthView();
              chained.reactiveView = firstUploader->reactiveView();
              chained.tcMaskView = firstUploader->tcMaskView();
              chained.exposureView = firstUploader->exposureView();
              chained.outputView = current->outputView();
              chained.historyReadView = current->historyReadView();
              chained.historyWriteView = current->historyWriteView();
              chained.reprojectedColorView = current->reprojectedColorView();
              chained.recurrentReadView = current->recurrentReadView();
              chained.recurrentWriteView = current->recurrentWriteView();
              chained.outputImage = current->outputImage();
              chained.historyReadImage = current->historyReadImage();
              chained.historyWriteImage = current->historyWriteImage();
              chained.reprojectedColorImage = current->reprojectedColorImage();
              chained.recurrentReadImage = current->recurrentReadImage();
              chained.recurrentWriteImage = current->recurrentWriteImage();
              // Secondary passes currently use neutral motion. Reusing their
              // temporal history without resolution-matched motion turns
              // compression/detail noise into a persistent lattice.
              chained.reset = true;
              chained.reactiveAverage = 1.0f;
              chained.hdr = in.hdr;
              chained.transfer = in.transfer;
              dr = harnessAt(pass)->dispatchFrame(chained);
              currentDispatchWaitCpuMs += dr.cpuWaitMs;
              if (dr.ok) {
                chainDispatchMs += dr.dispatchMs;
                chainGpuMs += dr.gpuMs;
              }
            }
            // Publish prior-jitter metadata only after every chained pass has
            // completed. The phase guard below rolls back on any later-pass
            // failure, so publishing after pass one would pair the next frame
            // with metadata from a frame that was never fully displayed.
            if (dr.ok) {
              previousJitterX = in.jitterX;
              previousJitterY = in.jitterY;
              hasPreviousJitter = true;
            }
            const double pipelineCpuMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - fsrPipelineStart)
                    .count();
            // Report the complete chain, not only the final pass. The latter
            // made an eight-pass frame appear to cost less than one pass.
            if (!runAsync) {
              lastFsr4DispatchMs_.store(chainDispatchMs,
                                        std::memory_order_release);
              lastFsr4GpuMs_.store(chainGpuMs,
                                   std::memory_order_release);
            }
            if (runAsync)
              chainGpuMs = lastFsr4GpuMs_.load(std::memory_order_acquire);
            emit fsr4StatusChanged();

            // The neural target remains hysteretic; presentation follows the
            // current fitted display size through a separate cached GPU
            // scaler. This avoids forcing a neural-resource rebuild for every
            // window resize while keeping Lanczos/bicubic selection active on
            // the actual FSR output.
            GpuImageUploader *presentationUploader =
                asyncSlots ? firstUploader : fsr4Uploader_.get();
            double presentationCpuMs = 0.0;
            if (!runAsync && dr.ok && !presentationFused &&
                presentationUploader) {
              const auto presentationStart = std::chrono::steady_clock::now();
              const bool presentationOk =
                  presentationUploader->dispatchPresentationScaler(displayW,
                                                                    displayH);
              presentationCpuMs = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() -
                                      presentationStart)
                                      .count();
              if (!presentationOk) {
                logWarn("PlaybackEngine: GPU presentation scaler failed");
                dr.ok = false;
              }
            }

            if (dr.ok && !runAsync) {
              // All chained passes share the same publication boundary. The
              // history index for every pass advances only after the final
              // presentation scaler succeeds, keeping failed frames out of
              // every temporal state chain.
              for (size_t pass = 0; pass < passCount; ++pass)
                uploaderAt(pass)->advanceHistory();
            }

            static uint32_t fsrFrameCounter = 0;
            const uint64_t sourceFrameIndex = fsrFrame->frameIndex;
            if (dr.ok)
              ++fsrFrameCounter;
            if (dr.ok && (fsrFrameCounter % fsrLogInterval) == 0u) {
              logInfo("PlaybackEngine: FSR4 frame pipelineCPU={:.3f}ms "
                      "dispatchCPU(all {} passes)={:.3f}ms "
                      "GPU(all {} passes)={:.3f}ms",
                      pipelineCpuMs, fsr4PassSizes_.size(), chainDispatchMs,
                      fsr4PassSizes_.size(), chainGpuMs);
              if (profileUploadEnv) {
                logInfo("PlaybackEngine: FSR4 upload color={:.3f}ms "
                        "motion={:.3f}ms neutral={:.3f}ms finalize={:.3f}ms",
                        colorUploadMs, motionUploadMs, neutralUploadMs,
                        uploadFinalizeMs);
              }
              if (profileTimingsEnv) {
                logInfo("PlaybackEngine: FSR4 stage-timing decodeCPU={:.3f}ms "
                        "uploadCPU={:.3f}ms presentationCPU={:.3f}ms "
                        "pipelineCPU={:.3f}ms dispatchCPU={:.3f}ms "
                        "recordCPU={:.3f}ms waitGPUCPU={:.3f}ms "
                        "GPU={:.3f}ms",
                        decodeCpuMs,
                        colorUploadMs + motionUploadMs + neutralUploadMs +
                            uploadFinalizeMs,
                        presentationCpuMs, pipelineCpuMs, chainDispatchMs,
                        dr.cpuRecordMs,
                        runAsync ? retiredGpuWaitCpuMs
                                 : currentDispatchWaitCpuMs,
                        chainGpuMs);
              }
            }

            if (!dr.ok) {
              // The side-buffer phase was prepared before dispatch so the
              // color upload and FSR metadata matched. If the complete chain
              // or presentation submission failed, return that phase to the
              // next attempt; otherwise dropped frames silently desynchronize
              // the synthetic sample sequence from submitted FSR frames.
              sideBufferSynth_.rollbackJitter(sideInputs.reset || reset);
              logWarn("PlaybackEngine: FSR4 dispatch failed: {}",
                      dr.failReason);
            } else {
              // Only a fully successful FSR chain consumes the prepared
              // jitter phase. Upstream: SideBufferSynth::update(). Downstream:
              // the next frame's Halton phase and temporal history contract.
              sideBufferSynth_.commitJitter();
              jitterPhaseGuard.markCommitted();
              // Commit only after the complete FSR chain succeeds. The next
              // decoded frame may then consume this frame's persistent state.
              temporalFrameContinuity.commit(sourceFrameIndex);
              static bool dumpedDecoder = false;
              if (!dumpedDecoder && dumpDecoderEnv &&
                  sourceFrameIndex >= dumpDecoderFrame) {
                // Record the metadata of the exact frame that reached the
                // FSR upload boundary. Upstream: VideoDecoder's resolved
                // DecodedVideoFrame. Downstream: the retained player log used
                // to explain color-matrix/transfer fallback decisions in
                // future 720p A/B captures. This is opt-in diagnostics only;
                // it does not alter the upload or reconstruction path.
                logInfo("PlaybackEngine: decoder_metadata frame={} source_frame={} "
                        "size={}x{} format={} range={} space={} transfer={} "
                        "primaries={} pts_us={} input_transfer_flag={} input_hdr={}",
                        fsrDf.frameIndex, df.frameIndex, fsrDf.width,
                        fsrDf.height, fsrDf.avFormat, fsrDf.colorRange,
                        fsrDf.colorSpace, fsrDf.colorTransfer,
                        fsrDf.colorPrimaries, fsrDf.ptsUs, in.transfer, in.hdr);
                std::vector<float> decoder;
                if (fsr4Harness_->readbackFinalAccum(decoder)) {
                  if (*dumpStageDirectory) {
                    std::filesystem::create_directories(dumpStageDirectory);
                    std::ofstream stageC(
                        std::filesystem::path(dumpStageDirectory) /
                            "stage-C-prepass.f32",
                        std::ios::binary | std::ios::trunc);
                    if (stageC)
                      stageC.write(
                          reinterpret_cast<const char *>(decoder.data()),
                          static_cast<std::streamsize>(decoder.size() *
                                                       sizeof(float)));
                    std::ofstream stageCMeta(
                        std::filesystem::path(dumpStageDirectory) /
                            "stage-C-prepass.json",
                        std::ios::trunc);
                    if (stageCMeta)
                      stageCMeta << "{\"frame\":" << sourceFrameIndex
                                 << ",\"floats\":" << decoder.size()
                                 << ",\"probe\":\"readbackFinalAccum\"}\n";
                  }
                  constexpr size_t kMaxDiagnosticPixels = 65536;
                  size_t decoderChannels =
                      std::getenv("TFORGE_FSR4_DUMP_PREFINAL") ? 16u : 8u;
                  if (const char *probeChannels = std::getenv(
                          "TFORGE_FSR4_DUMP_PREFINAL_CHANNELS")) {
                    char *end = nullptr;
                    const unsigned long parsed =
                        std::strtoul(probeChannels, &end, 10);
                    if (end != probeChannels && parsed > 0 && parsed <= 128)
                      decoderChannels = static_cast<size_t>(parsed);
                  }
                  const size_t pixelCount = decoder.size() / decoderChannels;
                  const size_t sampleStride =
                      std::max<size_t>(1, pixelCount / kMaxDiagnosticPixels);
                  std::array<std::vector<float>, 8> samples;
                  for (auto &channel : samples)
                    channel.reserve(std::min(pixelCount, kMaxDiagnosticPixels + 1));
                  for (size_t pixel = 0; pixel < pixelCount;
                       pixel += sampleStride) {
                    for (size_t c = 0; c < samples.size(); ++c) {
                      const float value =
                          decoder[pixel * decoderChannels + c];
                      if (std::isfinite(value))
                        samples[c].push_back(value);
                    }
                  }
                  logInfo("PlaybackEngine: {} frame={} pixels={} "
                          "channels={} sample_stride={}",
                          decoderChannels == 16 ? "pre-final" : "decoder",
                          sourceFrameIndex, pixelCount, decoderChannels,
                          sampleStride);
                  for (size_t c = 0; c < samples.size(); ++c) {
                    auto &channel = samples[c];
                    if (channel.empty())
                      continue;
                    std::sort(channel.begin(), channel.end());
                    const auto percentile = [&](double p) {
                      const size_t index = static_cast<size_t>(
                          p * static_cast<double>(channel.size() - 1));
                      return channel[index];
                    };
                    double sum = 0.0;
                    for (float value : channel)
                      sum += value;
                    logInfo("PlaybackEngine: decoder c{} min={:.5f} "
                            "p01={:.5f} p50={:.5f} p99={:.5f} max={:.5f} "
                            "mean={:.5f}",
                            c, channel.front(), percentile(0.01),
                            percentile(0.50), percentile(0.99), channel.back(),
                            sum / static_cast<double>(channel.size()));
                  }
                } else {
                  logWarn("PlaybackEngine: decoder readback failed");
                }
                dumpedDecoder = true;
              }
              // Opt-in diagnostic for inspecting the actual image
              // written by the native postpass. It is deliberately
              // one-shot and never substitutes for presentation.
              if (!fsr4DumpedOutput_ && dumpOutputEnv &&
                  sourceFrameIndex >= dumpOutputFrame) {
                uint32_t dumpW = 0, dumpH = 0;
                if (fsr4Uploader_->readbackOutput(fsr4Readback_, dumpW,
                                                  dumpH)) {
                  std::ofstream dump(dumpOutputPath,
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped native FSR4 output "
                            "frame={} {}x{} to {}",
                            sourceFrameIndex, dumpW, dumpH, dumpOutputPath);
                  }
                } else {
                  logWarn("PlaybackEngine: native FSR4 output readback failed");
                }
                fsr4DumpedOutput_ = true;
              }
              // Separate diagnostic for the image after the optional GPU
              // presentation scaler. Keeping this beside the pre-Qt output
              // dump makes presentation filtering measurable instead of
              // inferring it from the Qt surface.
              if (!fsr4DumpedPresentation_ && dumpPresentationEnv &&
                  sourceFrameIndex >= dumpOutputFrame) {
                uint32_t dumpW = 0, dumpH = 0;
                if (fsr4Uploader_->readbackPresentation(fsr4Readback_, dumpW,
                                                        dumpH)) {
                  std::ofstream dump(dumpPresentationPath,
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0;
                         i < static_cast<size_t>(dumpW) * dumpH; ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped presented FSR4 output "
                            "frame={} {}x{} to {}",
                            sourceFrameIndex, dumpW, dumpH, dumpPresentationPath);
                  }
                } else {
                  logWarn("PlaybackEngine: presented FSR4 output readback failed");
                }
                fsr4DumpedPresentation_ = true;
              }
              if (!fsr4DumpedRaw_ && dumpRawEnv &&
                  sourceFrameIndex >= dumpOutputFrame) {
                uint32_t dumpW = 0, dumpH = 0;
                if (firstUploader->readbackRaw(fsr4Readback_, dumpW, dumpH)) {
                  std::ofstream dump(dumpRawPath,
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped raw decoded image frame={} "
                            "{}x{} to {}",
                            sourceFrameIndex, dumpW, dumpH, dumpRawPath);
                  }
                } else {
                  logWarn("PlaybackEngine: raw image readback failed");
                }
                fsr4DumpedRaw_ = true;
              }
              // Multi-frame diagnostic used to measure temporal
              // stability. This reads the actual native RE output;
              // it is opt-in because readback stalls the GPU queue.
              if (dumpSequenceLimit > 0 &&
                  fsr4SequenceDumpCount_ <
                      static_cast<uint32_t>(dumpSequenceLimit)) {
                const bool pastWarmup =
                    fsr4SequenceFramesSeen_ >= dumpSequenceWarmup;
                if (pastWarmup) {
                  uint32_t dumpW = 0, dumpH = 0;
                  if (fsr4Uploader_->readbackOutput(fsr4Readback_, dumpW,
                                                    dumpH)) {
                  char sequenceName[64];
                  std::snprintf(sequenceName, sizeof(sequenceName),
                                "temporal_forge_fsr4_%04u.ppm",
                                fsr4SequenceDumpCount_);
                  const std::filesystem::path path =
                      std::filesystem::path(dumpSequenceDirectory) /
                      sequenceName;
                  std::ofstream dump(path, std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    dump.flush();
                    if (dump.good() && dumpMotionTextureEnv) {
                      // These files are raw diagnostic payloads: RG16F motion
                      // in source/model pixel units and one R8 validity byte
                      // per model pixel. Reading them back after the dispatch
                      // proves the exact dense textures bound to prepass,
                      // rather than inferring GPU contents from sparse seeds.
                      std::vector<uint8_t> denseMotionBytes;
                      std::vector<uint8_t> validityBytes;
                      uint32_t motionW = 0, motionH = 0;
                      uint32_t validityW = 0, validityH = 0;
                      const bool motionRead = fsr4Uploader_->readbackMotion(
                          denseMotionBytes, motionW, motionH);
                      const bool validityRead =
                          fsr4Uploader_->readbackMotionValidity(
                              validityBytes, validityW, validityH);
                      char motionName[64];
                      std::snprintf(motionName, sizeof(motionName),
                                    "dense_motion_%04u.rg16f",
                                    fsr4SequenceDumpCount_);
                      char validityName[64];
                      std::snprintf(validityName, sizeof(validityName),
                                    "dense_motion_validity_%04u.r8",
                                    fsr4SequenceDumpCount_);
                      if (motionRead && validityRead && motionW == validityW &&
                          motionH == validityH) {
                        std::ofstream motionDump(
                            std::filesystem::path(dumpMotionDirectory) /
                                motionName,
                            std::ios::binary | std::ios::trunc);
                        std::ofstream validityDump(
                            std::filesystem::path(dumpMotionDirectory) /
                                validityName,
                            std::ios::binary | std::ios::trunc);
                        if (motionDump && validityDump) {
                          motionDump.write(
                              reinterpret_cast<const char *>(
                                  denseMotionBytes.data()),
                              static_cast<std::streamsize>(
                                  denseMotionBytes.size()));
                          validityDump.write(
                              reinterpret_cast<const char *>(validityBytes.data()),
                              static_cast<std::streamsize>(validityBytes.size()));
                          logInfo("PlaybackEngine: dumped dense motion frame={} "
                                  "{}x{} rg16f={} bytes validity={} bytes",
                                  sourceFrameIndex, motionW, motionH,
                                  denseMotionBytes.size(), validityBytes.size());
                        }
                      } else {
                        logWarn("PlaybackEngine: dense motion readback failed "
                                "motion={} validity={} dims={}x{}/{}x{}",
                                motionRead, validityRead, motionW, motionH,
                                validityW, validityH);
                      }
                    }
                    if (dump.good() && dumpMotionSidecarEnv) {
                      char motionName[64];
                      std::snprintf(motionName, sizeof(motionName),
                                    "codec_motion_%04u.json",
                                    fsr4SequenceDumpCount_);
                      dumpCausalMotionFrame(
                          std::filesystem::path(dumpMotionDirectory) /
                              motionName,
                          *fsrFrame, sideInputs.reset,
                          sideInputs.histogramDelta, sideInputs.avgLumaDelta,
                          sideInputs.motionConfidence,
                          pastMotion, dumpW, dumpH,
                          fsr4SequenceDumpCount_);
                    }
                    // The FP16 reprojection diagnostic is an independent
                    // artifact. Do not make it conditional on the PPM stream
                    // state: presentation readback and history-warp readback
                    // are separate validation signals, and a late PPM write
                    // must not erase the motion diagnostic for that frame.
                    if (dumpReprojectedColorEnv) {
                      std::vector<uint8_t> reprojectedBytes;
                      uint32_t reprojW = 0, reprojH = 0;
                      const bool reprojRead =
                          fsr4Uploader_->readbackReprojectedColor(
                              reprojectedBytes, reprojW, reprojH);
                      char reprojName[64];
                      std::snprintf(reprojName, sizeof(reprojName),
                                    "reprojected_color_%04u.rgba16f",
                                    fsr4SequenceDumpCount_);
                      const size_t expectedBytes =
                          static_cast<size_t>(reprojW) * reprojH * 8u;
                      if (reprojRead && expectedBytes == reprojectedBytes.size()) {
                        std::ofstream reprojDump(
                            std::filesystem::path(dumpMotionDirectory) /
                                reprojName,
                            std::ios::binary | std::ios::trunc);
                        if (reprojDump) {
                          reprojDump.write(
                              reinterpret_cast<const char *>(
                                  reprojectedBytes.data()),
                              static_cast<std::streamsize>(
                                  reprojectedBytes.size()));
                          logInfo("PlaybackEngine: dumped reprojected color "
                                  "frame={} {}x{} rgba16f={} bytes",
                                  sourceFrameIndex, reprojW, reprojH,
                                  reprojectedBytes.size());
                        }
                      } else {
                        logWarn("PlaybackEngine: reprojected color readback "
                                "failed read={} dims={}x{} bytes={}",
                                reprojRead, reprojW, reprojH,
                                reprojectedBytes.size());
                      }
                    }
                    if (dump.good() && dumpEventTraceEnv) {
                      char eventName[64];
                      std::snprintf(eventName, sizeof(eventName),
                                    "event_trace_%04u.json",
                                    fsr4SequenceDumpCount_);
                      dumpEventTraceFrame(
                          std::filesystem::path(dumpEventTraceDirectory) /
                              eventName,
                          *fsrFrame, fsr4SequenceDumpCount_, reset,
                          sideInputs, ptsDeltaMs);
                    }
                  }
                  }
                  ++fsr4SequenceDumpCount_;
                }
                ++fsr4SequenceFramesSeen_;
              }
              if (!runAsync) {
                fsr4PublishedUploader_.store(presentationUploader,
                                              std::memory_order_release);
                // A successful neural publication must clear the native
                // passthrough selector; otherwise a previous 1:1 frame can
                // leave the render thread serving stale raw input forever.
                fsr4NativePassthrough_.store(false,
                                              std::memory_order_release);
                fsr4FrameReady_.store(true, std::memory_order_release);
                fsr4Upscaled = true;
                fsr4OutW = presentationUploader->outputW();
                fsr4OutH = presentationUploader->outputH();
              }
            }
            }
          }
        }
      }

      // EASU-only GPU upscale path (FSR4 off, Vulkan present).
      // Uploads the decoded frame and runs the EASU 2x pass so the display
      // gets a clean edge-adaptive upscale instead of pixelated bilinear.
      if (!fsr4Upscaled && easuOnlyMode_.load(std::memory_order_acquire) &&
          vkDevice_ != VK_NULL_HANDLE) {
        std::unique_lock dispatchLock(fsrDispatchMutex_);
        if (!fsrAbortRequested_.load(std::memory_order_acquire)) {
          // Lazy-init the uploader for EASU-only mode (native source, 2x
          // target — the target is unused since EASU writes directly).
          if (!fsr4Uploader_) {
            fsr4Uploader_ = std::make_unique<GpuImageUploader>();
            if (!fsr4Uploader_->init(vkPhysical_, vkDevice_, vkQueue_,
                                     vkQueueFamily_,
                                     vkPresentationQueueFamily_) ||
                !fsr4Uploader_->allocate(
                    static_cast<uint32_t>(df.width),
                    static_cast<uint32_t>(df.height),
                    static_cast<uint32_t>(df.width) * 2u,
                    static_cast<uint32_t>(df.height) * 2u) ||
                !fsr4Uploader_->transitionOutputToGeneral()) {
              logWarn("PlaybackEngine: EASU-only uploader init failed");
              fsr4Uploader_.reset();
              easuOnlyMode_.store(false, std::memory_order_release);
            }
          }
          if (fsr4Uploader_ &&
              (fsr4Uploader_->sourceW() != static_cast<uint32_t>(df.width) ||
               fsr4Uploader_->sourceH() != static_cast<uint32_t>(df.height))) {
            fsr4Uploader_->allocate(
                static_cast<uint32_t>(df.width),
                static_cast<uint32_t>(df.height),
                static_cast<uint32_t>(df.width) * 2u,
                static_cast<uint32_t>(df.height) * 2u);
            fsr4Uploader_->transitionOutputToGeneral();
          }
          if (fsr4Uploader_) {
            // Keep the spatial conversion and EASU dispatch in explicitly
            // ordered submissions.  The neural path supplies its own prefix
            // command buffer, while this fallback has no such handoff; an
            // image-level dependency makes the DRM/VAAPI path deterministic.
            const bool batchOk = fsr4Uploader_->beginFrameUploads(false);
            fsr4Uploader_->setPresentationScaler(
                qualityLabPresentationScaler(
                    qualityLabConfig_,
                    presentationScaler_.load(std::memory_order_acquire)));
            bool ok = fsr4Uploader_->uploadColor(df);
            if (ok)
              ok = fsr4Uploader_->dispatchEasu();
            fsr4Uploader_->endFrameUploads();
            if (ok) {
              fsr4Upscaled = true;
              fsr4OutW = fsr4Uploader_->easuW();
              fsr4OutH = fsr4Uploader_->easuH();
              fsr4FrameReady_.store(true, std::memory_order_release);
              // Keep the benchmark capture path available for the spatial
              // control as well as the neural path.  Without this, an Off
              // or EASU-only run silently produced no image, so quality
              // comparisons could not distinguish presentation/YUV issues
              // from FSR reconstruction issues.
              if (!fsr4DumpedOutput_ && dumpOutputEnv &&
                  df.frameIndex >= dumpOutputFrame) {
                uint32_t dumpW = 0, dumpH = 0;
                if (fsr4Uploader_->readbackEasu(fsr4Readback_, dumpW,
                                                dumpH)) {
                  std::ofstream dump(dumpOutputPath,
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0;
                         i < static_cast<size_t>(dumpW) * dumpH; ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped EASU output frame={} "
                            "{}x{} to {}",
                            df.frameIndex, dumpW, dumpH, dumpOutputPath);
                  }
                } else {
                  logWarn("PlaybackEngine: EASU output readback failed");
                }
                fsr4DumpedOutput_ = true;
              }
              if (!fsr4DumpedRaw_ && dumpRawEnv &&
                  df.frameIndex >= dumpOutputFrame) {
                uint32_t rawW = 0, rawH = 0;
                if (fsr4Uploader_->readbackRaw(fsr4Readback_, rawW, rawH)) {
                  std::ofstream dump(dumpRawPath,
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << rawW << ' ' << rawH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(rawW) * rawH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped EASU source frame={} "
                            "{}x{} to {}",
                            df.frameIndex, rawW, rawH, dumpRawPath);
                  }
                } else {
                  logWarn("PlaybackEngine: EASU source readback failed");
                }
                fsr4DumpedRaw_ = true;
              }
              (void)batchOk;
            } else {
              logWarn("PlaybackEngine: EASU-only dispatch failed");
            }
          }
        }
        dispatchLock.unlock();
      }

      // Build the render frame from either the upscaled RGBA or raw YUV.
      VideoFrameForRender rf;
      rf.ptsUs = fsrFrame->ptsUs;
      rf.durationUs = fsrFrame->durationUs;
      rf.keyframe = fsrFrame->keyframe;
      rf.frameIndex = fsrFrame->frameIndex;
      rf.reset = reset;
      if (fsr4Upscaled) {
        // FSR4 output is presented from the native Vulkan image by
        // VideoSurfaceItem. Do not copy the diagnostic readback
        // buffer into every queued frame; it is not the presentation
        // source and may contain data from an earlier diagnostic.
        rf.width = fsr4OutW;
        rf.height = fsr4OutH;
        rf.planes = 0;
      } else {
        // Raw decoded frame (graceful degradation / FSR4 disabled).
        rf.width = fsrFrame->width;
        rf.height = fsrFrame->height;
        rf.avFormat = fsrFrame->avFormat;
        rf.planes = fsrFrame->planes;
        for (int i = 0; i < fsrFrame->planes; ++i) {
          rf.linesize[i] = fsrFrame->linesize[i];
          rf.plane[i] = std::move(fsrFrame->plane[i]);
        }
      }

      // The hidden benchmark intentionally consumes decoded frames as
      // fast as possible to measure sustained GPU throughput without
      // source-clock pacing. Normal playback keeps the queue shallow.
      if (headlessBenchmarkEnv)
        continue;

      // Backpressure: keep the decoded queue shallow.
      {
        std::unique_lock lock(frameMutex_);
        frameCv_.wait(lock, [&] {
          return !running_.load() || seekPending_.load() ||
                 frames_.size() < kMaxFrames;
        });
        if (!running_.load())
          break;
        if (seekPending_.load())
          continue;
        // FSR4 presents from one shared Vulkan image, not from the queued
        // frame payload. Discard older metadata before publishing a new FSR
        // image so the renderer cannot pair frame N's PTS with frame N+1's
        // pixels.
        if (fsr4Upscaled)
          frames_.clear();
        frames_.push_back(std::move(rf));
        queuedFrames_.store(static_cast<uint32_t>(frames_.size()),
                            std::memory_order_release);
      }
    }
    if (pkt.isEof)
      endOfMediaPending_.store(true, std::memory_order_release);
  }
}

// audioDecodeLoop: the audio decode thread — pulls audio packets, decodes, and
//                  pushes interleaved float samples to the AudioSink.
//
// Runs on:  audioThread_ (started by startThreads). Exits when running_ is false.
// Calls:    AudioDecoder::sendPacket/receiveChunk, AudioSink::push (feeds the
//           ring buffer consumed by the master-clock device callback),
//           AudioSink::setStartPts on the first chunk.
// Notes:    Audio is the master clock (spec 01). If the ring is full, push drops
//           the remainder rather than blocking — the audio device owns the clock.
void PlaybackEngine::audioDecodeLoop() {
  bool firstAfterSeek = false;
  while (running_.load()) {
    Packet pkt;
    {
      std::unique_lock lock(pktMutex_);
      pktCv_.wait(lock, [&] {
        return !running_.load() || seekPending_.load() ||
               !audioPackets_.empty();
      });
      if (!running_.load())
        break;
      if (seekPending_.load())
        continue;
      if (audioPackets_.empty())
        continue;
      pkt = std::move(audioPackets_.front());
      audioPackets_.pop_front();
    }
    pktCv_.notify_all();

    if (pkt.isFlush) {
      adec_->flush();
      firstAfterSeek = true;
      continue;
    }

    adec_->sendPacket(pkt.isEof ? nullptr : pkt.av);
    DecodedAudioChunk chunk;
    while (adec_->receiveChunk(chunk)) {
      if (firstAfterSeek) {
        // Anchor the audio clock to the first decoded chunk's PTS.
        audio_.setStartPts(chunk.ptsUs);
        firstAfterSeek = false;
      }
      audio_.push(chunk.samples.data(), chunk.samples.size());
    }
  }
}

bool PlaybackEngine::consumeQueuedRenderFrame(VideoFrameForRender *out) {
  if (!playing_.load(std::memory_order_acquire))
    return false;
  if (queuedFrames_.load(std::memory_order_acquire) == 0)
    return false;

  const int64_t audioClock = audio_.clockUs();
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard lock(frameMutex_);
  if (frames_.empty())
    return false;

  if (audioClock >= 0) {
    while (frames_.size() > 1 && frames_[1].ptsUs <= audioClock)
      frames_.pop_front();
    if (frames_.empty())
      return false;
    const auto &f = frames_.front();
    if (f.ptsUs > audioClock + 500000)
      return false;
  } else {
    const auto &f = frames_.front();
    if (lastFramePts_ >= 0) {
      int64_t ptsDelta = f.ptsUs - lastFramePts_;
      auto wallElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             now - lastFrameWallTime_)
                             .count();
      if (wallElapsed < ptsDelta - 2000)
        return false;
    }
    while (frames_.size() > 1) {
      int64_t nextPts = frames_[1].ptsUs;
      auto nextPtsDelta = nextPts - lastFramePts_;
      auto nextWallElapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - lastFrameWallTime_)
              .count();
      if (nextPtsDelta <= nextWallElapsed + 2000)
        frames_.pop_front();
      else
        break;
    }
  }

  if (frames_.empty())
    return false;

  const int64_t ptsUs = frames_.front().ptsUs;
  if (out)
    *out = std::move(frames_.front());
  lastRenderedPtsUs_.store(ptsUs, std::memory_order_release);
  lastFramePts_ = ptsUs;
  lastFrameWallTime_ = now;
  frames_.pop_front();
  queuedFrames_.store(static_cast<uint32_t>(frames_.size()),
                      std::memory_order_release);
  frameCv_.notify_one();
  frameCounter_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool PlaybackEngine::takeRenderFrame(VideoFrameForRender &out) {
  return consumeQueuedRenderFrame(&out);
}

bool PlaybackEngine::advanceRenderFrame() {
  return consumeQueuedRenderFrame(nullptr);
}

void PlaybackEngine::sourceDimensions(int &w, int &h) const {
  std::lock_guard lock(infoMutex_);
  w = srcW_;
  h = srcH_;
}

bool PlaybackEngine::readbackLastDisplayedFrame(std::vector<uint8_t> &dst,
                                                uint32_t &outW,
                                                uint32_t &outH) {
  if (!fsr4Ready_.load(std::memory_order_acquire) || !fsr4Uploader_)
    return false;
  // Serialize against any in-flight dispatch + the render thread so the
  // uploader cannot be freed while we read from it. Same mutex teardown uses.
  std::lock_guard<std::mutex> lock(fsrDispatchMutex_);
  if (!fsr4Uploader_) return false;
  return fsr4Uploader_->readbackOutput(dst, outW, outH);
}

void PlaybackEngine::onPollTick() {
  if (hasMedia_.load())
    emit positionChanged();
  if (std::getenv("TFORGE_HEADLESS_BENCHMARK") != nullptr ||
      !endOfMediaPending_.load(std::memory_order_acquire) ||
      !playing_.load(std::memory_order_acquire))
    return;

  bool queueEmpty = false;
  {
    std::lock_guard lock(frameMutex_);
    queueEmpty = frames_.empty();
  }
  if (!queueEmpty)
    return;

  const qint64 duration = durationUs();
  const qint64 lastPts = lastRenderedPtsUs_.load(std::memory_order_acquire);
  // The decoder can drain a short tail before the UI has consumed the final
  // frame. Wait until the last displayed PTS is close to the container end so
  // automatic advancement never cuts off the last visible frame.
  if (duration > 0 && (lastPts < 0 || lastPts + 250'000 < duration))
    return;
  advancePlaylistAtEnd();
}

} // namespace temporal_forge
