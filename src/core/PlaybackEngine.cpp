// PlaybackEngine.cpp
#include "core/PlaybackEngine.hpp"
#include "backend/WeightBlob.hpp"
#include "util/FsrTargetMath.hpp"
#include "util/Log.hpp"

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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <utility>
namespace temporal_forge {

using namespace std::chrono_literals;

namespace {

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

LumaBuffer makeAnalysisLuma(const DecodedVideoFrame &frame) {
  LumaBuffer out;
  if (frame.planes <= 0 || frame.plane[0].empty() || frame.width <= 0 ||
      frame.height <= 0 || frame.linesize[0] <= 0)
    return out;
  out.width = std::min<uint32_t>(96u, static_cast<uint32_t>(frame.width));
  out.height = std::max<uint32_t>(1u, static_cast<uint32_t>(
      std::llround(static_cast<double>(frame.height) * out.width / frame.width)));
  out.data.resize(static_cast<size_t>(out.width) * out.height);
  const bool limited = frame.colorRange != AVCOL_RANGE_JPEG;
  const float scale = limited ? (1.0f / 219.0f) : (1.0f / 255.0f);
  const float bias = limited ? 16.0f : 0.0f;
  for (uint32_t y = 0; y < out.height; ++y) {
    const int sy = std::min(frame.height - 1,
                            static_cast<int>((static_cast<uint64_t>(y) * frame.height) /
                                             out.height));
    for (uint32_t x = 0; x < out.width; ++x) {
      const int sx = std::min(frame.width - 1,
                              static_cast<int>((static_cast<uint64_t>(x) * frame.width) /
                                               out.width));
      const float yValue = static_cast<float>(frame.plane[0][
          static_cast<size_t>(sy) * frame.linesize[0] + sx]);
      out.data[static_cast<size_t>(y) * out.width + x] =
          std::clamp((yValue - bias) * scale, 0.0f, 1.0f);
    }
  }
  return out;
}

float codecMotionConfidence(const std::vector<MvEntry> &mvs, int width,
                            int height) {
  if (width <= 0 || height <= 0 || mvs.empty())
    return mvs.empty() ? 0.5f : 0.0f;
  const double frameArea = static_cast<double>(width) * height;
  double covered = 0.0;
  double weightedMagnitude = 0.0;
  double weightedMagnitudeSq = 0.0;
  for (const MvEntry &mv : mvs) {
    const int blockW = std::max(1, static_cast<int>(mv.w));
    const int blockH = std::max(1, static_cast<int>(mv.h));
    const int x0 = std::clamp(static_cast<int>(mv.dstX), 0, width);
    const int y0 = std::clamp(static_cast<int>(mv.dstY), 0, height);
    const int x1 = std::clamp(static_cast<int>(mv.dstX) + blockW, 0, width);
    const int y1 = std::clamp(static_cast<int>(mv.dstY) + blockH, 0, height);
    const double area = static_cast<double>(std::max(0, x1 - x0)) *
                        std::max(0, y1 - y0);
    if (area <= 0.0) continue;
    const double magnitude = std::hypot(static_cast<double>(mv.mvX),
                                        static_cast<double>(mv.mvY)) /
                             std::hypot(static_cast<double>(width),
                                        static_cast<double>(height));
    covered += area;
    weightedMagnitude += area * magnitude;
    weightedMagnitudeSq += area * magnitude * magnitude;
  }
  if (covered <= 0.0) return 0.25f;
  const double coverage = std::clamp(covered / frameArea, 0.0, 1.0);
  const double mean = weightedMagnitude / covered;
  const double variance = std::max(0.0, weightedMagnitudeSq / covered - mean * mean);
  const double consistency = 1.0 / (1.0 + 18.0 * std::sqrt(variance));
  const double displacementTrust = 1.0 / (1.0 + 2.0 * mean);
  return static_cast<float>(std::clamp(0.25 + 0.75 * coverage * consistency *
                                                displacementTrust,
                                        0.0, 1.0));
}

std::vector<MvEntry> pastReferenceMotion(const std::vector<MvEntry> &mvs) {
  std::vector<MvEntry> past;
  past.reserve(mvs.size());
  for (const MvEntry &mv : mvs) {
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
    if (mv.source <= 0 && std::isfinite(mv.mvX) && std::isfinite(mv.mvY) &&
        std::hypot(mv.mvX, mv.mvY) <= maxDisplacement * 16.0f)
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
  output << std::setprecision(9);
  output << "{\n"
         << "  \"frameIndex\": " << frameIndex << ",\n"
         << "  \"ptsUs\": " << frame.ptsUs << ",\n"
         << "  \"reset\": " << (reset ? "true" : "false") << ",\n"
         << "  \"histogramDelta\": " << histogramDelta << ",\n"
         << "  \"avgLumaDelta\": " << avgLumaDelta << ",\n"
         << "  \"motionConfidence\": " << motionConfidence << ",\n"
         << "  \"motionAvailable\": "
         << (!causalMotion.empty() ? "true" : "false") << ",\n"
         << "  \"vectors\": [";
  for (size_t index = 0; index < causalMotion.size(); ++index) {
    const MvEntry &motion = causalMotion[index];
    if (index != 0) output << ',';
    output << "\n    {\"dstX\": " << static_cast<int>(motion.dstX)
           << ", \"dstY\": " << static_cast<int>(motion.dstY)
           << ", \"mvX\": " << motion.mvX
           << ", \"mvY\": " << motion.mvY
           << ", \"w\": " << static_cast<int>(motion.w)
           << ", \"h\": " << static_cast<int>(motion.h)
           << ", \"source\": " << static_cast<int>(motion.source) << '}';
  }
  if (!causalMotion.empty()) output << '\n';
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
         << "  \"transitionIndex\": "
         << (eventIndex == 0 ? "null" : std::to_string(eventIndex - 1))
         << ",\n"
         << "  \"ptsUs\": " << frame.ptsUs << ",\n"
         << "  \"ptsDeltaMs\": " << ptsDeltaMs << ",\n"
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

std::vector<MvEntry> scaleMotionToModel(const std::vector<MvEntry> &mvs,
                                        int sourceW, int sourceH,
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
    mv.mvX *= sx;
    mv.mvY *= sy;
    mv.w = static_cast<uint8_t>(std::clamp(
        std::lround(static_cast<float>(std::max(1, static_cast<int>(mv.w))) * sx),
        1l, 255l));
    mv.h = static_cast<uint8_t>(std::clamp(
        std::lround(static_cast<float>(std::max(1, static_cast<int>(mv.h))) * sy),
        1l, 255l));
    scaled.push_back(mv);
  }
  return scaled;
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
                                      uint32_t presentationQueueFamily) {
  vkPhysical_ = physical;
  vkDevice_ = device;
  vkQueue_ = queue;
  vkQueueFamily_ = queueFamily;
  vkPresentationQueueFamily_ = presentationQueueFamily;
  if (vkDevice_ == VK_NULL_HANDLE) {
    logInfo("PlaybackEngine: no Vulkan device — FSR4 upscaling disabled");
    return;
  }
  // We do not have the Vulkan instance here; the harness validates the
  // provided device/capability on create.
  vkCap_.valid = true;
  vkCap_.deviceName = "selected GPU";
  vkCap_.profile = Fsr4Profile::Int8Dot4;
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
    h->setQualityLabConfig(qualityLabConfig_);
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
  fsr4DumpedRaw_ = false;
  fsr4SequenceDumpCount_ = 0;
  fsr4SequenceFramesSeen_ = 0;
  fsr4DumpedPresentation_ = false;
  fsr4Ready_.store(true, std::memory_order_release);
  logInfo("PlaybackEngine: FSR4 path ready decoded {}x{} -> model {}x{} -> {}x{}",
          decodedW, decodedH, modelW, modelH, outW, outH);
  logInfo("PlaybackEngine: FSR4 progressive chain passes={}",
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
  image = published->presentationImage();
  width = published->presentationW();
  height = published->presentationH();
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
  DecodedVideoFrame pendingDecodedFrame;
  bool hasPendingDecodedFrame = false;
  static const bool forceResetEnv =
      std::getenv("TFORGE_FSR4_FORCE_RESET") != nullptr;
  static const char *jitterModeEnv = std::getenv("TFORGE_FSR4_JITTER_MODE");
  static const float controlledJitterStrength = [] {
    const char *value = std::getenv("TFORGE_FSR4_CONTROLLED_JITTER");
    return value ? std::clamp(std::strtof(value, nullptr), 0.0f, 1.5f) : 1.0f;
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
  static const bool dumpEventTraceEnv =
      std::getenv("TFORGE_FSR4_DUMP_EVENT_TRACE") != nullptr;
  // The default remains Current. Diagnostic runs can explicitly choose
  // off/reduced/controlled without changing history, motion, or reconstruction
  // rules; the environment value belongs in the capture manifest.
  if (jitterModeEnv && std::strcmp(jitterModeEnv, "off") == 0)
    sideBufferSynth_.setJitterMode(JitterMode::Off);
  else if (jitterModeEnv && std::strcmp(jitterModeEnv, "reduced") == 0)
    sideBufferSynth_.setJitterMode(JitterMode::Reduced);
  else if (jitterModeEnv && std::strcmp(jitterModeEnv, "controlled") == 0) {
    sideBufferSynth_.setJitterMode(JitterMode::Controlled);
    sideBufferSynth_.setControlledJitterStrength(controlledJitterStrength);
  } else {
    sideBufferSynth_.setJitterMode(JitterMode::Current);
  }
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
      pendingDecodedFrame = {};
      hasPendingDecodedFrame = false;
      firstAfterSeek = true;
      continue;
    }

    vdec_->sendPacket(pkt.isEof ? nullptr : pkt.av);
    DecodedVideoFrame df;
    double decodeCpuMs = 0.0;
    while (true) {
      if (hasPendingDecodedFrame) {
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
      DecodedVideoFrame nextDecodedFrame;
      if (vdec_->receiveFrame(nextDecodedFrame)) {
        pendingDecodedFrame = std::move(nextDecodedFrame);
        hasPendingDecodedFrame = true;
      }
      // First frame after a seek/new-file must reset history (spec 02).
      const bool timestampDiscontinuity =
          lastAnalysisPtsUs_ >= 0 &&
          (df.ptsUs <= lastAnalysisPtsUs_ ||
           df.ptsUs - lastAnalysisPtsUs_ > 5'000'000);
      // A future-reference vector is only safe after an independently
      // validated future frame. This player remains causal, so timestamp
      // discontinuities reset the temporal state rather than attempting to
      // bridge an uncertain reference chain.
      const bool reset = firstAfterSeek || timestampDiscontinuity;
      firstAfterSeek = false;

      const float ptsDeltaMs = lastAnalysisPtsUs_ >= 0 && df.ptsUs >= lastAnalysisPtsUs_
          ? static_cast<float>(df.ptsUs - lastAnalysisPtsUs_) / 1000.0f
          : 0.0f;
      lastAnalysisPtsUs_ = df.ptsUs;
      promoteStableFsrViewport();
      sideBufferSynth_.setRenderSize(
          fsrTargetViewportW_.load(std::memory_order_acquire),
          fsrTargetViewportH_.load(std::memory_order_acquire));
      const std::vector<MvEntry> pastMotion =
          pastReferenceMotion(df.motionVectors);
      const LumaBuffer analysisLuma = makeAnalysisLuma(df);
      const float futureAnalysisConfidence =
          hasPendingDecodedFrame
              ? lookaheadConfidence(df, pendingDecodedFrame)
              : 1.0f;
      // Apply the UI/benchmark value on the decode thread immediately before
      // synthesizing side inputs. The setter is intentionally atomic because
      // it can be called from Qt's UI thread while this loop is running; the
      // synthesizer itself is owned and updated by this loop.
      sideBufferSynth_.setJitterStrength(
          jitterStrength_.load(std::memory_order_acquire));
      const SideBufferInputs sideInputs = sideBufferSynth_.update(
          analysisLuma, ptsDeltaMs, reset,
          codecMotionConfidence(pastMotion, df.width, df.height) *
              futureAnalysisConfidence);
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
      const DecodedVideoFrame *fsrFrame = &df;
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
        const Size2D nativeTarget = nativeInt8FixedTarget(
            alignEven(static_cast<uint32_t>(df.width)),
            alignEven(static_cast<uint32_t>(df.height)));
        // Keep the neural target independent of the window. The current
        // fitted viewport is only the presentation target, so a resize can
        // reuse the neural resources and dispatch the cheap cached scaler.
        uint32_t neuralTargetW = nativeTarget.width;
        uint32_t neuralTargetH = nativeTarget.height;
        uint32_t displayW = std::max(
            2u, fsrViewportW_.load(std::memory_order_acquire));
        uint32_t displayH = std::max(
            2u, fsrViewportH_.load(std::memory_order_acquire));
        const auto fitToViewport = [&](uint32_t viewportW,
                                       uint32_t viewportH) {
          const double fit = std::min(
              static_cast<double>(viewportW) / df.width,
              static_cast<double>(viewportH) / df.height);
          return std::pair<uint32_t, uint32_t>{
              std::max(2u, alignEven(static_cast<uint32_t>(
                                         std::round(df.width * fit)))),
              std::max(2u, alignEven(static_cast<uint32_t>(
                                         std::round(df.height * fit))))};
        };
        if (forcedViewport) {
          const auto viewport = fsrViewportForBenchmark(
              std::max(2u, fsrTargetViewportW_.load(std::memory_order_acquire)),
              std::max(2u, fsrTargetViewportH_.load(std::memory_order_acquire)));
          if (nativeTarget.width == viewport.first &&
              nativeTarget.height == viewport.second) {
            neuralTargetW = nativeTarget.width;
            neuralTargetH = nativeTarget.height;
          } else {
            const auto fitted = fitToViewport(viewport.first, viewport.second);
            neuralTargetW = fitted.first;
            neuralTargetH = fitted.second;
          }
          displayW = neuralTargetW;
          displayH = neuralTargetH;
        } else {
          const auto fitted = fitToViewport(displayW, displayH);
          displayW = fitted.first;
          displayH = fitted.second;
        }
        const uint32_t fsrInputW = std::max(
            2u, alignEven(static_cast<uint32_t>(std::round(
                                  neuralTargetW / selectedScale))));
        const uint32_t fsrInputH = std::max(
            2u, alignEven(static_cast<uint32_t>(std::round(
                                  neuralTargetH / selectedScale))));
        // Never enlarge a decoded frame before FSR. When the multiplier's
        // nominal input is larger than the decoded source, clamp the model
        // input to source dimensions; the GPU prefilter is then identity-size
        // and no second softening filter is introduced.
        const uint32_t modelW = std::min(fsrInputW, static_cast<uint32_t>(df.width));
        const uint32_t modelH = std::min(fsrInputH, static_cast<uint32_t>(df.height));
        const DecodedVideoFrame &fsrDf = *fsrFrame;
        const std::vector<MvEntry> temporalMotion = scaleMotionToModel(
            pastReferenceMotion(fsrDf.motionVectors), fsrDf.width, fsrDf.height,
            modelW, modelH);
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
               ((uint32_t)fsrDf.width != configuredInput->sourceW() ||
                (uint32_t)fsrDf.height != configuredInput->sourceH()))) {
            if (!initFsr4Path(fsrDf.width, fsrDf.height,
                              static_cast<int>(modelW),
                              static_cast<int>(modelH))) {
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
          const bool asyncSlots =
              singlePass && fsr4InFlightUploader_ && fsr4InFlightHarness_ &&
              std::getenv("TFORGE_FSR4_DISABLE_INFLIGHT") == nullptr;
          GpuImageUploader *firstUploader = nullptr;
          Fsr4DispatchHarness *firstHarness = nullptr;
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
              firstUploader->completeDeferredFrameUploads();
              if (completed.ok) {
                firstUploader->advanceHistory();
                if (!firstUploader->dispatchPresentationScaler(displayW,
                                                                 displayH)) {
                  logWarn("PlaybackEngine: in-flight presentation scaler "
                          "failed");
                } else {
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
          const bool initializeNeutral =
              sideInputs.reset || !fsr4FrameReady_.load(std::memory_order_acquire);
          bool uploadOk = firstUploader->beginFrameUploads(!initializeNeutral);
          if (uploadOk) {
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
            const auto motionUploadStart = std::chrono::steady_clock::now();
            uploadOk &= firstUploader->uploadMotion(temporalMotion);
            motionUploadMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - motionUploadStart)
                    .count();

            // These are compatibility inputs inherited from the game
            // model, not changing video-frame data. Rebuilding Sobel
            // depth and a 3x3 reactive field over every source pixel
            // stalled the decode thread and injected frame-to-frame
            // noise into the reconstruction. Initialize stable neutral
            // values when temporal state resets; color and codec motion
            // are the only per-frame uploads.
            if (initializeNeutral) {
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
            // EASU 2x is NOT run before the FSR4 neural dispatch — the CNN
            // was designed to take native-res input and do its own upscaling.
            // EASU runs only for the FSR4-off display path (easuOnlyMode_).
            const auto uploadFinalizeStart = std::chrono::steady_clock::now();
            // The decoded color and motion uploads belong to firstUploader.
            // This is an intermediate uploader for chained passes, so ending
            // the final output uploader's batch leaves the real batch open
            // and makes the next frame fail beginFrameUploads().
            uploadOk &=
                firstUploader->endFrameUploads(&uploadCommandBuffer);
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

            FrameDispatchInput in;
            in.prefixCommandBuffer = uploadCommandBuffer;
            in.colorView = firstUploader->colorView();
            in.colorImage = firstUploader->colorImage();
            in.sourceDisplayView = firstUploader->rawPresentationView();
            in.sourceDisplayImage = firstUploader->rawPresentationImage();
            in.motionView = firstUploader->motionView();
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
            in.reset = multipass || sideInputs.reset || forceResetEnv;
            in.jitterX = sideInputs.jitterX;
            in.jitterY = sideInputs.jitterY;
            in.frameTimeMs = ptsDeltaMs > 0.0f ? ptsDeltaMs : 16.6667f;
            in.historyConfidence = sideInputs.motionConfidence;
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
                                  !dumpRawEnv;
            auto dr = runAsync ? firstHarness->dispatchFrameAsync(in)
                               : firstHarness->dispatchFrame(in);
            double chainDispatchMs = dr.dispatchMs;
            double chainGpuMs = dr.gpuMs;
            if (!runAsync) {
              firstUploader->completeDeferredFrameUploads();
              if (dr.ok)
                firstUploader->advanceHistory();
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
              chained.hdr = in.hdr;
              chained.transfer = in.transfer;
              dr = harnessAt(pass)->dispatchFrame(chained);
              if (dr.ok) {
                chainDispatchMs += dr.dispatchMs;
                chainGpuMs += dr.gpuMs;
                current->advanceHistory();
              }
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
            if (!runAsync && dr.ok &&
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
                        "GPU={:.3f}ms",
                        decodeCpuMs,
                        colorUploadMs + motionUploadMs + neutralUploadMs +
                            uploadFinalizeMs,
                        presentationCpuMs, pipelineCpuMs, chainDispatchMs,
                        chainGpuMs);
              }
            }

            if (!dr.ok) {
              logWarn("PlaybackEngine: FSR4 dispatch failed: {}",
                      dr.failReason);
            } else {
              static bool dumpedDecoder = false;
              if (!dumpedDecoder && dumpDecoderEnv &&
                  sourceFrameIndex >= dumpDecoderFrame) {
                std::vector<float> decoder;
                if (fsr4Harness_->readbackFinalAccum(decoder)) {
                  constexpr size_t kMaxDiagnosticPixels = 65536;
                  const size_t pixelCount = decoder.size() / 8;
                  const size_t sampleStride =
                      std::max<size_t>(1, pixelCount / kMaxDiagnosticPixels);
                  std::array<std::vector<float>, 8> samples;
                  for (auto &channel : samples)
                    channel.reserve(std::min(pixelCount, kMaxDiagnosticPixels + 1));
                  for (size_t pixel = 0; pixel < pixelCount;
                       pixel += sampleStride) {
                    for (size_t c = 0; c < samples.size(); ++c) {
                      const float value = decoder[pixel * 8 + c];
                      if (std::isfinite(value))
                        samples[c].push_back(value);
                    }
                  }
                  logInfo("PlaybackEngine: decoder frame={} pixels={} "
                          "sample_stride={}",
                          sourceFrameIndex, pixelCount, sampleStride);
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
                          pastReferenceMotion(fsrFrame->motionVectors), dumpW,
                          dumpH, fsr4SequenceDumpCount_);
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
                fsr4FrameReady_.store(true, std::memory_order_release);
                fsr4Upscaled = true;
                fsr4OutW = presentationUploader->outputW();
                fsr4OutH = presentationUploader->outputH();
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
      rf.ptsUs = df.ptsUs;
      rf.durationUs = df.durationUs;
      rf.keyframe = df.keyframe;
      rf.frameIndex = df.frameIndex;
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
        rf.width = df.width;
        rf.height = df.height;
        rf.avFormat = df.avFormat;
        rf.planes = df.planes;
        for (int i = 0; i < df.planes; ++i) {
          rf.linesize[i] = df.linesize[i];
          rf.plane[i] = std::move(df.plane[i]);
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
