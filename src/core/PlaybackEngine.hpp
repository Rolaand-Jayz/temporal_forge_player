// PlaybackEngine.hpp — orchestrates demux + decode + audio + frame pacing.
//
// spec 01 process model:
//   Main/UI Thread      (Qt)        -> window events, controls, settings
//   Playback Thread     (this)      -> demux scheduling, audio clock, pacing
//   Decode Threads      (this)      -> video + audio packet decode
//
// Clocking (spec 01): audio clock primary, video PTS fallback. Never slow
// audio. Late video frames reduce quality / drop for A/V sync only.
//
// This class is exposed to QML as a singleton-ish controller. The actual
// GPU upload + present happens on the render thread owned by VideoSurface.
#pragma once
#include "config/SettingsStore.hpp"
#include "config/QualityLabConfig.hpp"
#include "media/Demuxer.hpp"
#include "media/VideoDecoder.hpp"
#include "media/AudioDecoder.hpp"
#include "audio/AudioSink.hpp"
#include "backend/BackendSelector.hpp"
#include "backend/WeightBlob.hpp"
#include "backend/GpuCapabilityProbe.hpp"
#include "render/GpuImageUploader.hpp"
#include "render/Fsr4DispatchHarness.hpp"
#include "render/SideBufferSynth.hpp"
#include "util/FsrTargetMath.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <chrono>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vulkan/vulkan.h>

struct SwsContext;

namespace temporal_forge {

struct VideoFrameForRender {
    int64_t ptsUs = 0;
    int64_t durationUs = 0;
    int width = 0;
    int height = 0;
    int avFormat = 0;
    std::vector<uint8_t> plane[4];
    int linesize[4] = {0,0,0,0};
    int planes = 0;
    bool keyframe = false;
    bool reset = false;          // history reset requested (seek/scene cut)
    uint64_t frameIndex = 0;
};

class PlaybackEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY mediaChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY mediaChanged)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY positionChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaChanged)
    Q_PROPERTY(QVariantMap mediaInfo READ mediaInfoQml NOTIFY mediaChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(bool fsr4Enabled READ fsr4Enabled NOTIFY fsr4StatusChanged)
    Q_PROPERTY(bool fsr4Active READ fsr4Active NOTIFY fsr4StatusChanged)
    Q_PROPERTY(bool compareEnabled READ compareEnabled WRITE setCompareEnabled NOTIFY compareEnabledChanged)
    Q_PROPERTY(bool fsr4ProofPassed READ fsr4ProofPassed NOTIFY fsr4StatusChanged)
    Q_PROPERTY(double lastFsr4DispatchMs READ lastFsr4DispatchMs NOTIFY fsr4StatusChanged)
    Q_PROPERTY(uint fsr4OutputWidth READ fsr4OutputWidth NOTIFY fsr4StatusChanged)
    Q_PROPERTY(uint fsr4OutputHeight READ fsr4OutputHeight NOTIFY fsr4StatusChanged)
    Q_PROPERTY(QStringList playlist READ playlist NOTIFY playlistChanged)
    Q_PROPERTY(int playlistIndex READ playlistIndex NOTIFY playlistChanged)
    Q_PROPERTY(int playlistCount READ playlistCount NOTIFY playlistChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY playlistChanged)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY playlistChanged)
    // Monotonic counter incremented whenever a new displayable frame is
    // produced. QML uses this to cache-bust the image provider source.
    Q_PROPERTY(int frameCounter READ frameCounter NOTIFY frameCounterChanged)

public:
    explicit PlaybackEngine(QObject* parent = nullptr);
    ~PlaybackEngine();

    // --- QML-facing API ---
    Q_INVOKABLE void openUrl(const QUrl& url);
    Q_INVOKABLE void openPlaylist(const QStringList& entries);
    Q_INVOKABLE void appendPlaylist(const QStringList& entries);
    Q_INVOKABLE void selectPlaylist(int index);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void clearPlaylist();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekUs(qint64 us);
    Q_INVOKABLE void close();

    // Wire the FSR4 real-frame playback path to the Vulkan device. Called once
    // at startup from main.cpp after VulkanContext::init(). Loads the RE weight
    // blob + probes the GPU capability. When not called (or probe fails), the
    // engine plays raw decoded frames with no upscaling (graceful degradation).
    void setVulkanHandles(VkPhysicalDevice physical, VkDevice device,
                          VkQueue queue, uint32_t queueFamily,
                          uint32_t presentationQueueFamily);
    void setQualityLabConfig(const QualityLabConfig &config) {
        qualityLabConfig_ = config;
    }
    void setSharpness(float sharpness) { sharpness_.store(sharpness, std::memory_order_release); }
    void setPresentationScaler(int scaler) {
        presentationScaler_.store(std::clamp(scaler, 0, 4), std::memory_order_release);
    }
    void setJitterStrength(float v) { jitterStrength_.store(v, std::memory_order_release); }
    bool compareEnabled() const { return compareEnabled_.load(std::memory_order_acquire); }
    void setCompareEnabled(bool enabled);

    // Enable/disable the FSR4 upscaling path. When enabled and Vulkan handles
    // are set + weights loaded, decoded frames are upscaled before display.
    void setFsr4Enabled(bool enabled);
    void setFsrViewport(uint32_t width, uint32_t height, float scale);
    bool fsr4NativeOutput(VkImage& image, uint32_t& width, uint32_t& height) const;
    bool fsr4RawOutput(VkImage& image, uint32_t& width, uint32_t& height) const;

    // Tear down the live FSR4 harness/uploader and wait for the GPU to idle
    // so the Qt render thread stops referencing those images before we free
    // them. Safe to call from the UI thread while the decode thread may be
    // mid-dispatch (the decode loop observes fsrAbortRequested_).
    void teardownFsr4Path();

    bool playing() const { return playing_.load(std::memory_order_acquire); }
    bool hasMedia() const { return hasMedia_.load(std::memory_order_acquire); }
    qint64 durationUs() const;
    qint64 positionUs() const;
    QString mediaTitle() const;
    QVariantMap mediaInfoQml() const;
    int volume() const;
    void setVolume(int v);
    bool muted() const;
    void setMuted(bool m);
    int frameCounter() const { return frameCounter_.load(); }

    // --- render thread pulls the next frame to display ---
    // Returns false if no frame is ready / paused / EOF.
    // The returned frame's data is moved out (one-shot consume).
    bool takeRenderFrame(VideoFrameForRender& out);
    bool advanceRenderFrame();

    // Current source dims (0 if no media).
    void sourceDimensions(int& w, int& h) const;

    // Read the last displayed frame back to host memory as RGBA8. Used by the
    // screenshot / clipboard feature. Returns false when there is no media or
    // FSR4 output is not ready (in which case `dst` is left untouched).
    // Performs a synchronous GPU readback: callers must not invoke this from
    // the render thread or while a dispatch is in flight (it would stall the
    // decode thread). The screenshot path runs this once per click — cheap.
    bool readbackLastDisplayedFrame(std::vector<uint8_t>& dst,
                                    uint32_t& outW, uint32_t& outH);

    [[nodiscard]] BackendSelector& backendSelector() { return selector_; }
    [[nodiscard]] uint64_t historyResets() const { return historyResets_.load(); }
    [[nodiscard]] uint64_t sceneCutsDetected() const { return sceneCuts_.load(); }
    [[nodiscard]] float lastReactiveAverage() const { return lastReactive_.load(); }
    [[nodiscard]] float lastMotionConfidence() const { return lastMotionConf_.load(); }
    // FSR4 real-path telemetry (for the debug overlay / Phase B HUD).
    [[nodiscard]] bool fsr4Enabled() const { return fsr4Enabled_.load(std::memory_order_acquire); }
    [[nodiscard]] bool fsr4Active() const {
        return fsr4Ready_.load(std::memory_order_acquire) && fsr4Enabled();
    }
    [[nodiscard]] bool fsr4ProofPassed() const { return fsr4ProofPassed_.load(std::memory_order_acquire); }
    [[nodiscard]] double lastFsr4DispatchMs() const { return lastFsr4DispatchMs_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t fsr4OutputWidth() const { return fsr4OutW_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t fsr4OutputHeight() const { return fsr4OutH_.load(std::memory_order_acquire); }
    QStringList playlist() const { return playlist_; }
    int playlistIndex() const { return playlistIndex_; }
    int playlistCount() const { return playlist_.size(); }
    bool hasNext() const { return playlistIndex_ >= 0 && playlistIndex_ + 1 < playlist_.size(); }
    bool hasPrevious() const { return playlistIndex_ > 0; }

signals:
    void stateChanged();
    void mediaChanged();
    void positionChanged();
    void volumeChanged();
    void errorOccurred(const QString& message);
    void frameCounterChanged();
    void fsr4StatusChanged();
    void compareEnabledChanged();
    void playlistChanged();

private:
    // Thread loops
    void demuxLoop();
    void videoDecodeLoop();
    void audioDecodeLoop();

    void stopThreads();
    void startThreads();
    void onPollTick();
    bool consumeQueuedRenderFrame(VideoFrameForRender* out);

    // Helpers
    bool fillMediaInfoQml();
    void promoteStableFsrViewport();
    bool openUrlInternal(const QUrl& url);
    static QUrl playlistUrl(const QString& entry);
    static QString playlistEntry(const QUrl& url);
    bool openPlaylistIndex(int index);
    void advancePlaylistAtEnd();

    // media
    std::unique_ptr<Demuxer> demux_;
    std::unique_ptr<VideoDecoder> vdec_;
    std::unique_ptr<AudioDecoder> adec_;
    AudioSink audio_;
    SideBufferSynth sideBufferSynth_;
    int64_t lastAnalysisPtsUs_ = -1;

    // Playlist state is owned by the Qt/UI thread. Decode-thread EOF is
    // reduced to endOfMediaPending_; onPollTick performs the actual switch so
    // open/close and Vulkan teardown stay on the same thread as other controls.
    QStringList playlist_;
    int playlistIndex_ = -1;
    std::atomic<bool> endOfMediaPending_{false};

    // upscaling backend
    BackendSelector selector_;

    // --- FSR4 real-frame playback path ---
    // Owns the Vulkan compute harness + image uploader used to run the 27-pass
    // neural upscaler on each decoded frame. Lazy-initialized when Vulkan
    // handles are provided AND the weight blob loads AND the GPU probe passes.
    VkPhysicalDevice vkPhysical_ = VK_NULL_HANDLE;
    VkDevice       vkDevice_     = VK_NULL_HANDLE;
    VkQueue        vkQueue_      = VK_NULL_HANDLE;
    uint32_t       vkQueueFamily_= ~0u;
    uint32_t       vkPresentationQueueFamily_ = ~0u;
    GpuCapability  vkCap_{};
    std::unique_ptr<GpuImageUploader> fsr4Uploader_;
    std::unique_ptr<Fsr4DispatchHarness> fsr4Harness_;
    // A second complete resource set allows the decoder to upload/record the
    // next frame while the previous FSR submission is executing. It is used
    // only for the ordinary single-pass path; chained passes remain serial.
    std::unique_ptr<GpuImageUploader> fsr4InFlightUploader_;
    std::unique_ptr<Fsr4DispatchHarness> fsr4InFlightHarness_;
    std::atomic<GpuImageUploader *> fsr4PublishedUploader_{nullptr};
    GpuImageUploader *fsr4LastSubmittedUploader_ = nullptr;
    uint32_t fsr4NextDispatchSlot_ = 0;
    // Optional progressive chain. The final pass remains in the legacy
    // members above so presentation/readback keep one stable owner.
    std::vector<std::unique_ptr<GpuImageUploader>> fsr4IntermediateUploaders_;
    std::vector<std::unique_ptr<Fsr4DispatchHarness>> fsr4IntermediateHarnesses_;
    std::vector<Size2D> fsr4PassSizes_;
    Fsr4BlobView   fsr4Blob_;
    std::vector<uint8_t> fsr4BlobStorage_; // owns the loaded blob bytes
    Fsr4Preset fsr4LoadedBlobPreset_ = Fsr4Preset::Quality;
    std::atomic<bool> fsr4Enabled_{false}; // user wants upscaling
    std::atomic<bool> fsr4Ready_{false};   // weights + harness + uploader initialized
    // Exact native-size selections bypass neural reconstruction and publish
    // the uploader's decoded presentation image without changing any upscale.
    std::atomic<bool> fsr4NativePassthrough_{false};
    // EASU-only mode: when FSR4 is off but Vulkan is present, keep the
    // uploader alive to run the EASU 2x GPU upscale and display it. This
    // replaces the pixelated CPU/bilinear off-path with GPU-accelerated
    // edge-adaptive scaling.
    std::atomic<bool> easuOnlyMode_{false};
    // Set by the UI thread when it wants the decode loop to stop touching
    // Vulkan FSR4 resources immediately (file switch, preset change, close).
    // The decode loop polls this between frame uploads and aborts the current
    // dispatch attempt without waiting on the GPU fence.
    std::atomic<bool> fsrAbortRequested_{false};
    // Serializes UI-thread FSR4 teardown/reconfigure against the decode
    // thread's per-frame dispatch AND the Qt render thread's readout of the
    // current output image. The decode loop holds this mutex for the
    // duration of one dispatch (upload + dispatch + history advance); the
    // UI thread holds it while destroying/recreating the harness/uploader;
    // the render-thread accessors (fsr4NativeOutput/fsr4RawOutput) hold it
    // only long enough to copy the VkImage + dims out. Combined with
    // vkQueueWaitIdle this guarantees no in-flight command buffer references
    // freed Vulkan resources and no render thread reads a dangling pointer.
    mutable std::mutex fsrDispatchMutex_;
    std::atomic<bool> fsr4FrameReady_{false}; // postpass completed at least once
    std::atomic<bool> fsr4ProofRun_{false};    // proof has executed (pass or fail)
    std::atomic<bool> fsr4ProofPassed_{false}; // proof passed
    std::atomic<uint32_t> fsr4OutW_{0}, fsr4OutH_{0}; // current output dims
    std::atomic<uint32_t> fsrViewportW_{1280}, fsrViewportH_{720};
    std::atomic<uint32_t> fsrTargetViewportW_{1280}, fsrTargetViewportH_{720};
    std::atomic<int64_t> fsrViewportChangedUs_{0};
    std::atomic<float> fsrScale_{2.0f};
    std::atomic<float> sharpness_{0.3f};
    std::atomic<int> presentationScaler_{2};
    std::atomic<float> jitterStrength_{1.0f};
    QualityLabConfig qualityLabConfig_{};
    std::atomic<bool> compareEnabled_{false};
    float fsr4AppliedSharpness_ = -1.0f;
    bool fsr4AppliedCompareEnabled_ = false;
    std::atomic<double> lastFsr4DispatchMs_{0.0};
    std::atomic<double> lastFsr4GpuMs_{0.0};
    // Reusable readback buffer (avoids per-frame alloc).
    std::vector<uint8_t> fsr4Readback_;
    bool fsr4DumpedOutput_ = false;
    bool fsr4DumpedRaw_ = false;
    uint32_t fsr4SequenceDumpCount_ = 0;
    // Startup frames can be excluded from an opt-in temporal capture so the
    // benchmark measures steady-state output rather than resource warm-up.
    uint32_t fsr4SequenceFramesSeen_ = 0;
    bool fsr4DumpedPresentation_ = false;
    // Initializes fsr4Harness_ + fsr4Uploader_ for the current source dims.
    bool initFsr4Path(int decodedW, int decodedH, int modelW, int modelH);

    // threads
    std::thread demuxThread_;
    std::thread videoThread_;
    std::thread audioThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> hasMedia_{false};
    std::atomic<bool> seekPending_{false};
    std::atomic<int64_t> seekTargetUs_{0};

    // packet queue (small; demux feeds decoders)
    std::mutex pktMutex_;
    std::condition_variable pktCv_;
    std::deque<Packet> videoPackets_;
    std::deque<Packet> audioPackets_;
    static constexpr size_t kMaxVideoPackets = 64;
    static constexpr size_t kMaxAudioPackets = 64;

    // decoded frame queue (shallow, spec 01: GPU frame queue 2-4 frames)
    // Here we keep a small decoded-video queue before presentation.
    std::mutex frameMutex_;
    std::condition_variable frameCv_;
    std::deque<VideoFrameForRender> frames_;
    static constexpr size_t kMaxFrames = 8;
    std::atomic<uint32_t> queuedFrames_{0};

    // audio chunk queue
    std::mutex audioMutex_;
    std::deque<DecodedAudioChunk> audioChunks_;
    static constexpr size_t kMaxAudioChunks = 16;

    // misc
    QTimer pollTimer_;
    std::atomic<int64_t> lastRenderedPtsUs_{-1};
    mutable std::mutex infoMutex_;
    QVariantMap mediaInfoQml_;
    QString mediaTitle_;
    int64_t durationUs_ = 0;
    int srcW_ = 0, srcH_ = 0;
    std::atomic<int> volume_{100};
    std::atomic<bool> muted_{false};
    std::atomic<int> frameCounter_{0};
    // Wall-clock pacing for when audio clock is unavailable.
    std::chrono::steady_clock::time_point lastFrameWallTime_{};
    int64_t lastFramePts_ = -1;
    std::atomic<uint64_t> historyResets_{0};
    std::atomic<uint64_t> sceneCuts_{0};
    std::atomic<float> lastReactive_{0.0f};
    std::atomic<float> lastMotionConf_{1.0f};
};

} // namespace temporal_forge
