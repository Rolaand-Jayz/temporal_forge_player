// PlaybackEngine.cpp
#include "core/PlaybackEngine.hpp"
#include "backend/WeightBlob.hpp"
#include "util/FsrTargetMath.hpp"
#include "util/Log.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
namespace temporal_forge {

using namespace std::chrono_literals;

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
  logInfo("PlaybackEngine: Vulkan handles set; FSR4 will be initialized on "
          "first frame");
}

void PlaybackEngine::setFsr4Enabled(bool enabled) {
  if (fsr4Enabled_.load(std::memory_order_acquire) == enabled)
    return;
  fsr4Enabled_.store(enabled, std::memory_order_release);
  fsr4FrameReady_.store(false, std::memory_order_release);
  if (!enabled) {
    // Disabling: tear down the live GPU resources now so the Qt render
    // thread cannot keep sampling an image we are about to drop. The
    // harness/uploader are recreated lazily on the next enable.
    teardownFsr4Path();
    fsr4ProofRun_.store(false, std::memory_order_release);
    fsr4ProofPassed_.store(false, std::memory_order_release);
    lastFsr4DispatchMs_.store(0.0, std::memory_order_release);
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
  fsr4Uploader_.reset();
  fsr4Harness_.reset();
  fsr4IntermediateUploaders_.clear();
  fsr4IntermediateHarnesses_.clear();
  fsr4PassSizes_.clear();
  fsr4OutW_.store(0, std::memory_order_release);
  fsr4OutH_.store(0, std::memory_order_release);
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
bool PlaybackEngine::initFsr4Path(int srcW, int srcH) {
  if (vkDevice_ == VK_NULL_HANDLE || !vkCap_.valid)
    return false;
  if (srcW <= 0 || srcH <= 0)
    return false;

  // FSR renders at the selected multiplier target. The scene graph scales
  // this GPU image to the player viewport; the viewport is not a neural
  // dispatch target.
  float scale = fsrScale_.load();
  if (const char *env = std::getenv("TFORGE_FSR4_FORCE_SCALE")) {
    char *end = nullptr;
    const float forced = std::strtof(env, &end);
    if (end != env && std::isfinite(forced) && forced >= 1.0f)
      scale = forced;
  }
  uint32_t outW =
      std::max(2u, alignEven(static_cast<uint32_t>(std::round(srcW * scale))));
  uint32_t outH =
      std::max(2u, alignEven(static_cast<uint32_t>(std::round(srcH * scale))));
  if (scale >= 2.99f) {
    const Size2D nativeTarget = nativeInt8UltraPerformanceTarget(
        static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH));
    if (nativeTarget.width != 0) {
      outW = nativeTarget.width;
      outH = nativeTarget.height;
    }
  }

  // Load the packed RE weight blob once. Every chained pass uses the same
  // immutable weights but owns independent temporal/output resources.
  if (fsr4BlobStorage_.empty()) {
    std::filesystem::path blobFile;
    for (auto p : {
             std::filesystem::path(
                 "/home/rolaandjayz/ZCodeProject/RE-of-FSR-4.1.0-Upscaling-1.0/"
                 "extracted/v410_initializers/quality.bin"),
             std::filesystem::path("RE-of-FSR-4.1.0-Upscaling-1.0/extracted/"
                                   "v410_initializers/quality.bin"),
             std::filesystem::path("../RE-of-FSR-4.1.0-Upscaling-1.0/extracted/"
                                   "v410_initializers/quality.bin")}) {
      if (std::filesystem::exists(p)) { blobFile = p; break; }
    }
    if (blobFile.empty()) {
      logWarn("PlaybackEngine: FSR4 weight blob not found; upscaling disabled");
      return false;
    }
    auto loaded = WeightBlobLoader::load(Fsr4Preset::Quality, blobFile.string());
    if (!loaded.ok) {
      logWarn("PlaybackEngine: FSR4 weight blob load failed ({}); upscaling disabled",
              loaded.failReason);
      return false;
    }
    fsr4Blob_ = WeightBlobLoader::view(loaded);
    fsr4BlobStorage_ = std::move(loaded.data);
    logInfo("PlaybackEngine: FSR4 weight blob loaded ({} bytes)",
            fsr4BlobStorage_.size());
  }

  const Size2D sourceSize{alignEven(static_cast<uint32_t>(srcW)),
                          alignEven(static_cast<uint32_t>(srcH))};
  const Size2D targetSize{outW, outH};
  const bool nativeFixedTarget =
      nativeInt8UltraPerformanceTarget(sourceSize.width, sourceSize.height)
          .width != 0 &&
      targetSize.width == nativeInt8UltraPerformanceTarget(
                              sourceSize.width, sourceSize.height)
                              .width &&
      targetSize.height == nativeInt8UltraPerformanceTarget(
                               sourceSize.width, sourceSize.height)
                               .height;
  std::vector<Size2D> requested;
  bool repeatNativeChain = false;
  if (const char *env = std::getenv("TFORGE_FSR4_CHAIN_PASSES")) {
    char *end = nullptr;
    const long count = std::strtol(env, &end, 10);
    if (end != env && count > 0)
      requested = fsrProgressivePassSizes(sourceSize.width, sourceSize.height,
                                           targetSize.width, targetSize.height);
    if (end != env && count > 0) {
      if (nativeFixedTarget && count > 1) {
        repeatNativeChain = true;
        requested.assign(static_cast<size_t>(count), targetSize);
      }
      // The final requested target is always one pass. The override counts
      // total neural passes, not intermediate sizes.
      const size_t total = static_cast<size_t>(count);
      if (repeatNativeChain)
        ;
      else if (total == 1)
        requested.clear();
      else if (total - 1 < requested.size())
        requested.resize(total - 1);
    }
  } else {
    requested = fsrProgressivePassSizes(sourceSize.width, sourceSize.height,
                                         targetSize.width, targetSize.height);
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
      fsr4IntermediateHarnesses_.size() + 1 == requested.size();
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
    fsr4IntermediateUploaders_.clear();
    fsr4IntermediateHarnesses_.clear();
    fsr4PassSizes_ = requested;
  }

  auto createPass = [&](uint32_t passSourceW, uint32_t passSourceH,
                        const Size2D &passTarget,
                        std::unique_ptr<Fsr4DispatchHarness> &h,
                        std::unique_ptr<GpuImageUploader> &u) -> bool {
    logInfo("PlaybackEngine: FSR4 pass {}x{} -> {}x{}", passSourceW,
            passSourceH, passTarget.width, passTarget.height);
    h = std::make_unique<Fsr4DispatchHarness>();
    if (!h->init(vkPhysical_, vkDevice_, vkQueue_, vkQueueFamily_, vkCap_))
      return false;
    Fsr4DispatchResources r{};
    r.sourceWidth = passSourceW;
    r.sourceHeight = passSourceH;
    r.outputWidth = passTarget.width;
    r.outputHeight = passTarget.height;
    if (!h->allocateResources(r) || !h->uploadWeights(fsr4Blob_))
      return false;
    u = std::make_unique<GpuImageUploader>();
    if (!u->init(vkPhysical_, vkDevice_, vkQueue_, vkQueueFamily_,
                 vkPresentationQueueFamily_) ||
        !u->allocate(passSourceW, passSourceH, passTarget.width,
                     passTarget.height) || !u->transitionOutputToGeneral())
      return false;
    u->setSharpness(sharpness_.load(std::memory_order_acquire));
    u->setCompareEnabled(compareEnabled_.load(std::memory_order_acquire));
    return true;
  };

  if (!dimensionsMatch) {
    // Preserve the decoder's exact first-pass dimensions. Later targets are
    // even-aligned by the progressive planner, but changing the first input
    // dimensions makes the upload path reject valid odd-width video frames.
    uint32_t passSourceW = static_cast<uint32_t>(srcW);
    uint32_t passSourceH = static_cast<uint32_t>(srcH);
    for (size_t i = 0; i < requested.size(); ++i) {
      if (i + 1 == requested.size()) {
        if (!createPass(passSourceW, passSourceH, requested[i], fsr4Harness_,
                        fsr4Uploader_)) {
          fsr4PassSizes_.clear();
          return false;
        }
      } else {
        fsr4IntermediateHarnesses_.push_back(nullptr);
        fsr4IntermediateUploaders_.push_back(nullptr);
        if (!createPass(passSourceW, passSourceH, requested[i],
                        fsr4IntermediateHarnesses_.back(),
                        fsr4IntermediateUploaders_.back())) {
          fsr4PassSizes_.clear();
          return false;
        }
      }
      if (repeatNativeChain) {
        passSourceW = sourceSize.width;
        passSourceH = sourceSize.height;
      } else {
        passSourceW = requested[i].width;
        passSourceH = requested[i].height;
      }
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

  fsr4OutW_.store(outW, std::memory_order_release);
  fsr4OutH_.store(outH, std::memory_order_release);
  fsr4FrameReady_.store(false, std::memory_order_release);
  fsr4DumpedOutput_ = false;
  fsr4DumpedRaw_ = false;
  fsr4SequenceDumpCount_ = 0;
  fsr4Ready_.store(true, std::memory_order_release);
  logInfo("PlaybackEngine: FSR4 path ready {}x{} -> {}x{}", srcW, srcH, outW,
          outH);
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
  const float oldScale = fsrScale_.load();
  fsrViewportW_.store(width);
  fsrViewportH_.store(height);
  fsrScale_.store(scale);
  // spec 02: window size only affects presentation, never the FSR target.
  // Only a scale (preset) change requires the decode loop to re-initialize
  // the harness with new output dimensions. A pure window resize must NOT
  // trigger teardown — that would force a multi-second weight-blob reload
  // + pipeline rebuild on every resize and stall the UI on vkQueueWaitIdle.
  if (scale != oldScale) {
    fsr4Ready_.store(false, std::memory_order_release);
    fsr4FrameReady_.store(false, std::memory_order_release);
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
  if (!fsr4Enabled_.load(std::memory_order_acquire) ||
      !fsr4Ready_.load(std::memory_order_acquire) ||
      !fsr4FrameReady_.load(std::memory_order_acquire) || !fsr4Uploader_)
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
  image = fsr4Uploader_->outputImage();
  width = fsr4Uploader_->outputW();
  height = fsr4Uploader_->outputH();
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
  if (!fsr4Ready_.load(std::memory_order_acquire) || !fsr4Uploader_)
    return false;
  image = fsr4Uploader_->rawPresentationImage();
  width = fsr4Uploader_->sourceW();
  height = fsr4Uploader_->sourceH();
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

// openUrl: open and begin playing a media file.
//
// Called by: QML (Q_INVOKABLE) from the file dialog / drag-drop / command line.
// Calls:     close() (fully stops threads + drains GPU + tears down FSR4 so the
//            next file starts clean), Demuxer::open, VideoDecoder/AudioDecoder::open,
//            startThreads, AudioSink::start.
// Notes:     Regression lesson 2026-07-20: close() alone is enough — do NOT also
//            call stopThreads() here (it re-joined already-joined threads).
//            Emits errorOccurred on open failure; mediaChanged/stateChanged on success.
void PlaybackEngine::openUrl(const QUrl &url) {
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
    return;
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
  static const bool forceResetEnv =
      std::getenv("TFORGE_FSR4_FORCE_RESET") != nullptr;
  static const bool dumpDecoderEnv =
      std::getenv("TFORGE_FSR4_DUMP_DECODER") != nullptr;
  static const uint32_t dumpDecoderFrame = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_DECODER_FRAME");
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static const bool dumpOutputEnv =
      std::getenv("TFORGE_FSR4_DUMP_OUTPUT") != nullptr;
  static const bool dumpRawEnv = std::getenv("TFORGE_FSR4_DUMP_RAW") != nullptr;
  static const uint32_t dumpOutputFrame = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_OUTPUT_FRAME");
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static const char *dumpOutputPath = [] {
    const char *value = std::getenv("TFORGE_FSR4_DUMP_OUTPUT_PATH");
    return value && *value ? value : "/tmp/temporal_forge_fsr4_output.ppm";
  }();
  static const bool headlessBenchmarkEnv =
      std::getenv("TFORGE_HEADLESS_BENCHMARK") != nullptr;
  static const bool profileUploadEnv =
      std::getenv("TFORGE_FSR4_PROFILE_UPLOAD") != nullptr;
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
      firstAfterSeek = true;
      continue;
    }

    vdec_->sendPacket(pkt.isEof ? nullptr : pkt.av);
    DecodedVideoFrame df;
    while (vdec_->receiveFrame(df)) {
      // First frame after a seek/new-file must reset history (spec 02).
      const bool reset = firstAfterSeek;
      firstAfterSeek = false;

      // --- FSR4 real-frame upscaling path (Phase A) ---
      // Run on `df` BEFORE building the render frame, because the upload
      // needs the YUV planes which would otherwise be moved into rf.
      // This is an experimental RE-derived image, but it must be
      // presented so visual quality can be evaluated. Validation/proof
      // status remains separate from presentation policy.
      bool fsr4Upscaled = false;
      uint32_t fsr4OutW = 0, fsr4OutH = 0;
      if (fsr4Enabled_.load(std::memory_order_acquire) &&
          vkDevice_ != VK_NULL_HANDLE &&
          !fsrAbortRequested_.load(std::memory_order_acquire)) {
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
               ((uint32_t)df.width != configuredInput->sourceW() ||
                (uint32_t)df.height != configuredInput->sourceH()))) {
            if (!initFsr4Path(df.width, df.height)) {
              fsr4Enabled_.store(
                  false,
                  std::memory_order_release); // init failed — stop retrying
            }
          }
        }
        if (dispatchLock.owns_lock() &&
            !fsrAbortRequested_.load(std::memory_order_acquire) &&
            fsr4Ready_.load(std::memory_order_acquire) && fsr4Uploader_ &&
            fsr4Harness_) {
          GpuImageUploader *firstUploader =
              fsr4IntermediateUploaders_.empty()
                  ? fsr4Uploader_.get()
                  : fsr4IntermediateUploaders_.front().get();
          Fsr4DispatchHarness *firstHarness =
              fsr4IntermediateHarnesses_.empty()
                  ? fsr4Harness_.get()
                  : fsr4IntermediateHarnesses_.front().get();
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
              reset || !fsr4FrameReady_.load(std::memory_order_acquire);
          bool uploadOk = firstUploader->beginFrameUploads(!initializeNeutral);
          if (uploadOk) {
            const auto colorUploadStart = std::chrono::steady_clock::now();
            uploadOk = firstUploader->uploadColor(df);
            colorUploadMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - colorUploadStart)
                    .count();
            // 2. Upload side-buffer textures (real modes).
            if (df.planes > 0) {
              // Luma is only needed for the reset-time side buffers.
            }
            const auto motionUploadStart = std::chrono::steady_clock::now();
            uploadOk &= firstUploader->uploadMotion(df.motionVectors);
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
              sbs.motionVectors = &df.motionVectors;
              if (df.planes > 0) {
                sbs.luma = df.plane[0].data();
                sbs.lumaWidth = df.width;
                sbs.lumaHeight = df.height;
                sbs.lumaLinesize = df.linesize[0];
              }
              sbs.reactiveAverage = 0.0f;
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
            in.motionView = firstUploader->motionView();
            in.depthView = firstUploader->depthView();
            in.reactiveView = firstUploader->reactiveView();
            in.tcMaskView = firstUploader->tcMaskView();
            in.exposureView = firstUploader->exposureView();
            in.outputView = firstUploader->outputView();
            in.historyReadView = firstUploader->historyReadView();
            in.historyWriteView = firstUploader->historyWriteView();
            in.recurrentReadView = firstUploader->recurrentReadView();
            in.recurrentWriteView = firstUploader->recurrentWriteView();
            in.outputImage = firstUploader->outputImage();
            in.historyReadImage = firstUploader->historyReadImage();
            in.historyWriteImage = firstUploader->historyWriteImage();
            in.recurrentReadImage = firstUploader->recurrentReadImage();
            in.recurrentWriteImage = firstUploader->recurrentWriteImage();
            // Independent chained passes do not share motion/history at the
            // same resolution. Reset the first pass too, otherwise its
            // temporal reprojection shifts the source seen by later passes.
            const bool multipass = fsr4PassSizes_.size() > 1;
            in.reset = multipass || reset || forceResetEnv;
            in.hdr = false;

            auto dr = firstHarness->dispatchFrame(in);
            double chainDispatchMs = dr.dispatchMs;
            double chainGpuMs = dr.gpuMs;
            firstUploader->completeDeferredFrameUploads();
            if (dr.ok)
              firstUploader->advanceHistory();

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
              chained.recurrentReadView = current->recurrentReadView();
              chained.recurrentWriteView = current->recurrentWriteView();
              chained.outputImage = current->outputImage();
              chained.historyReadImage = current->historyReadImage();
              chained.historyWriteImage = current->historyWriteImage();
              chained.recurrentReadImage = current->recurrentReadImage();
              chained.recurrentWriteImage = current->recurrentWriteImage();
              // Secondary passes currently use neutral motion. Reusing their
              // temporal history without resolution-matched motion turns
              // compression/detail noise into a persistent lattice.
              chained.reset = true;
              chained.hdr = false;
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
            lastFsr4DispatchMs_.store(chainDispatchMs,
                                      std::memory_order_release);
            emit fsr4StatusChanged();

            static uint32_t fsrFrameCounter = 0;
            const uint32_t fsrFrameIndex = fsrFrameCounter;
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
            }

            if (!dr.ok) {
              logWarn("PlaybackEngine: FSR4 dispatch failed: {}",
                      dr.failReason);
            } else {
              static bool dumpedDecoder = false;
              if (!dumpedDecoder && dumpDecoderEnv &&
                  fsrFrameIndex >= dumpDecoderFrame) {
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
                          fsrFrameIndex, pixelCount, sampleStride);
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
                  fsrFrameIndex >= dumpOutputFrame) {
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
                            fsrFrameIndex, dumpW, dumpH, dumpOutputPath);
                  }
                } else {
                  logWarn("PlaybackEngine: native FSR4 output readback failed");
                }
                fsr4DumpedOutput_ = true;
              }
              if (!fsr4DumpedRaw_ && dumpRawEnv) {
                uint32_t dumpW = 0, dumpH = 0;
                if (fsr4Uploader_->readbackRaw(fsr4Readback_, dumpW, dumpH)) {
                  std::ofstream dump("/tmp/temporal_forge_fsr4_raw.ppm",
                                     std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                    logInfo("PlaybackEngine: dumped raw decoded image {}x{} to "
                            "/tmp/temporal_forge_fsr4_raw.ppm",
                            dumpW, dumpH);
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
                uint32_t dumpW = 0, dumpH = 0;
                if (fsr4Uploader_->readbackOutput(fsr4Readback_, dumpW,
                                                  dumpH)) {
                  char path[128];
                  std::snprintf(path, sizeof(path),
                                "/tmp/temporal_forge_fsr4_%04u.ppm",
                                fsr4SequenceDumpCount_);
                  std::ofstream dump(path, std::ios::binary | std::ios::trunc);
                  if (dump) {
                    dump << "P6\n" << dumpW << ' ' << dumpH << "\n255\n";
                    for (size_t i = 0; i < static_cast<size_t>(dumpW) * dumpH;
                         ++i)
                      dump.write(reinterpret_cast<const char *>(
                                     fsr4Readback_.data() + i * 4),
                                 3);
                  }
                }
                ++fsr4SequenceDumpCount_;
              }
              fsr4FrameReady_.store(true, std::memory_order_release);
              fsr4Upscaled = true;
              fsr4OutW = fsr4Uploader_->outputW();
              fsr4OutH = fsr4Uploader_->outputH();
            }
          }
        }
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
}

} // namespace temporal_forge
