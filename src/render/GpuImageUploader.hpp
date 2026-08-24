// GpuImageUploader.hpp — converts decoded frames into the VkImage set the FSR4
// prepass consumes, and reads back the postpass output.
//
// Owns one image per prepass input slot (spec §3.1 universal binding map):
//   b1 color    (rgb10_a2) — current sharpened FSR model-color frame
//   b2 motion   (rg16f)    — codec/block/zero motion vectors
//   b3 depth    (r32f)     — flat or edge-lite compatibility depth
//   b4 reactive (r8)       — cheap luma/confidence mask
//   b5 tcMask   (r8)       — cleared (spec default)
//   exposure    (r16f 1x1) — auto-exposure scalar
//
// Also owns the output + history images written by the postpass:
//   u_output     (rgba8 outputW×outputH) — native display image
//   u_historyOut (rgba16f outputW×outputH) — temporal model history
//   u_reprojectedColor (rgba16f outputW×outputH) — one shared causal resolve
//
// All images are device-local. Uploads go through a host-visible staging buffer
// + vkCmdCopyBufferToImage. One VkFence serializes uploads (synchronous path —
// matches the harness dispatch model).
//
// Reallocated only when source or output dimensions change.
#pragma once
#include "media/VideoDecoder.hpp"
#include "render/VulkanUtil.hpp"

#include <atomic>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
struct SwsContext;

namespace temporal_forge {

// Side-buffer synthesis inputs produced CPU-side (from SideBufferSynth + the
// decoded frame's motion vectors + luma). The uploader turns these into the
// GPU textures the prepass reads.
struct SideBufferSource {
  // Codec motion vectors (empty → caller should use block-match or zero mode).
  const std::vector<MvEntry> *motionVectors = nullptr;

  // Luma plane pointer + dims + linesize, used for EdgeLite depth + reactive.
  const uint8_t *luma = nullptr;
  int lumaWidth = 0;
  int lumaHeight = 0;
  int lumaLinesize = 0;

  // SideBufferSynth scalars (already computed per-frame by PlaybackEngine).
  float reactiveAverage = 0.0f; // global reactive gain
  float exposureScalar = 1.0f;  // auto-exposure (1.0 = neutral)
};

struct GpuImageUploader {
  bool init(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
            uint32_t queueFamily,
            uint32_t presentationQueueFamily = VK_QUEUE_FAMILY_IGNORED);
  void destroy();

  // (Re)allocate all images for the given source + output dims. No-op if
  // dims unchanged since last call. Returns false on allocation failure
  // (caller should fall back to spatial).
  bool allocate(uint32_t sourceW, uint32_t sourceH, uint32_t outputW,
                uint32_t outputH, uint32_t modelW = 0,
                uint32_t modelH = 0);

  // Upload color (YUV -> sharpened FSR model-color RGB10_A2). Returns false on
  // failure. avFormat is an AVPixelFormat cast to int.
  bool uploadColor(const DecodedVideoFrame &frame);
  void setSharpness(float sharpness) {
    sharpness_.store(sharpness, std::memory_order_release);
  }
  void setPresentationScaler(int scaler) {
    presentationScaler_.store(scaler, std::memory_order_release);
  }
  void setCompareEnabled(bool enabled) {
    compareEnabled_.store(enabled, std::memory_order_release);
  }

  // Upload motion texture (rg16f) from codec MVs. If `mvs` is empty the
  // image is cleared to zero (motion-mode callers decide what to pass).
  bool uploadMotion(const std::vector<MvEntry> &mvs);

  // Upload depth (r32f) — EdgeLite: Sobel edge detect on luma; Flat: cleared.
  bool uploadDepthEdgeLite(const SideBufferSource &s);
  bool uploadDepthFlat();

  // Upload reactive (r8) — CheapAuto/Aggressive: luma-variance ×
  // reactiveAverage.
  bool uploadReactive(const SideBufferSource &s, bool aggressive);

  // Clear tcMask to zero (spec default).
  bool clearTcMask();

  // Upload exposure 1x1 r16f.
  bool uploadExposure(float scalar);

  // Batch all per-frame texture uploads. Passing deferredCmd records and ends
  // the command buffer without submitting it so the harness can submit upload
  // and inference together under one fence. Call completeDeferredFrameUploads
  // after that fence signals.
  bool beginFrameUploads(bool allowBatch = true);
  bool endFrameUploads(VkCommandBuffer *deferredCmd = nullptr);
  void completeDeferredFrameUploads();

  // --- input image views (bound by the harness prepass) ---
  // Conversion writes color_ directly at native/model resolution.  When a
  // prefilter is needed it also writes its result into color_; sourceModel_
  // is only the prefilter's private source and must never be exposed to FSR.
  VkImageView colorView() const { return color_.view; }
  VkImage colorImage() const { return color_.image; }
  VkImageView rawPresentationView() const { return rawPresentation_.view; }
  VkImage rawPresentationImage() const { return rawPresentation_.image; }

  // EASU 2x pre-scale output (2x native res). When EASU is active, the FSR4
  // dispatch / display path reads this instead of the native colorView.
  VkImageView easuColorView() const { return easuImage_.view; }
  VkImage easuColorImage() const { return easuImage_.image; }
  uint32_t easuW() const { return easuImage_.width; }
  uint32_t easuH() const { return easuImage_.height; }
  bool easuReady() const { return easuActive_; }
  VkImageView yPlaneView() const { return yPlane_.view; }
  VkImageView uPlaneView() const { return uPlane_.view; }
  VkImageView vPlaneView() const { return vPlane_.view; }
  VkImageView motionView() const { return motion_.view; }
  VkImageView depthView() const { return depth_.view; }
  VkImageView reactiveView() const { return reactive_.view; }
  VkImageView tcMaskView() const { return tcMask_.view; }
  VkImageView exposureView() const { return exposure_.view; }

  // --- output images (written by postpass, read by readback) ---
  VkImageView outputView() const { return output_.view; }
  VkImage outputImage() const { return output_.image; }
  VkImage presentationImage() const {
    return presentation_.image != VK_NULL_HANDLE ? presentation_.image
                                                  : output_.image;
  }
  uint32_t presentationW() const {
    return presentation_.image != VK_NULL_HANDLE ? presentation_.width
                                                 : output_.width;
  }
  uint32_t presentationH() const {
    return presentation_.image != VK_NULL_HANDLE ? presentation_.height
                                                 : output_.height;
  }
  VkImageView historyReadView() const {
    return history_[historyIndex_.load(std::memory_order_acquire)].view;
  }
  VkImageView historyWriteView() const {
    return history_[historyIndex_.load(std::memory_order_acquire) ^ 1u].view;
  }
  VkImageView recurrentReadView() const {
    return recurrent_[historyIndex_.load(std::memory_order_acquire)].view;
  }
  VkImageView recurrentWriteView() const {
    return recurrent_[historyIndex_.load(std::memory_order_acquire) ^ 1u].view;
  }
  VkImage historyReadImage() const {
    return history_[historyIndex_.load(std::memory_order_acquire)].image;
  }
  VkImage historyWriteImage() const {
    return history_[historyIndex_.load(std::memory_order_acquire) ^ 1u].image;
  }
  VkImage recurrentReadImage() const {
    return recurrent_[historyIndex_.load(std::memory_order_acquire)].image;
  }
  VkImage recurrentWriteImage() const {
    return recurrent_[historyIndex_.load(std::memory_order_acquire) ^ 1u].image;
  }
  VkImageView reprojectedColorView() const { return reprojectedColor_.view; }
  VkImage reprojectedColorImage() const { return reprojectedColor_.image; }
  // The decoder thread publishes a completed history image while the Qt
  // render thread reads the published handle.  Make that hand-off explicit;
  // a plain uint32 here was a data race during playback/toggle operations.
  void advanceHistory() {
    historyIndex_.fetch_xor(1u, std::memory_order_release);
  }

  // Read the postpass output (rgba8) back into an RGBA8888 QImage-shaped
  // buffer at output dims. `dst` must be outputW*outputH*4 bytes.
  bool readbackOutput(std::vector<uint8_t> &dst, uint32_t &outW,
                      uint32_t &outH);
  // Read the image after the optional GPU presentation scaler. When the
  // presentation target is the same size as the reconstruction output, this
  // returns the reconstruction image without inserting a second copy.
  bool readbackPresentation(std::vector<uint8_t> &dst, uint32_t &outW,
                            uint32_t &outH);
  // Read the spatial EASU image. It is a separate image from the neural
  // output, so using readbackOutput() here would legitimately return the
  // untouched (black) neural target.
  bool readbackEasu(std::vector<uint8_t> &dst, uint32_t &outW,
                   uint32_t &outH);
  bool readbackRaw(std::vector<uint8_t> &dst, uint32_t &outW, uint32_t &outH);

  // Transition output + history images from UNDEFINED to GENERAL layout.
  // Must be called once after allocate() before the first dispatch.
  bool transitionOutputToGeneral();

  // dispatchEasu: run the FSR1-EASU compute pass to scale the native-res
  //              uploaded color image to a 2x intermediate. Records into the
  //              active frame-upload command buffer (must be called between
  //              beginFrameUploads + uploadColor and endFrameUploads).
  //              Called by: PlaybackEngine::videoDecodeLoop.
  //              After this returns, easuColorView() is valid for the next
  //              pipeline stage (FSR4 dispatch or display).
  bool dispatchEasu();
  bool dispatchPresentationScaler(uint32_t width, uint32_t height);

  uint32_t sourceW() const { return srcW_; }
  uint32_t sourceH() const { return srcH_; }
  uint32_t modelW() const { return modelW_; }
  uint32_t modelH() const { return modelH_; }
  uint32_t outputW() const { return outW_; }
  uint32_t outputH() const { return outH_; }

private:
  struct ImportedDrmFrame {
    bool valid = false;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int layers = 0;
    int planes = 0;
    int objects = 0;
    int width = 0;
    int height = 0;
    int colorRange = 0;
    int colorSpace = 0;
    int fd = -1;
    VkSubresourceLayout planeLayouts[4]{};
  };
  struct ImportedDrmRuntime {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView yView = VK_NULL_HANDLE;
    VkImageView uvView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  bool ensureStaging(VkDeviceSize size);
  bool ensureMotionVectorBuffer(size_t vectorCount);
  bool beginUploadCmd();
  bool endUploadCmd();
  // Transition an image layout and copy a staging buffer region into it.
  bool copyBufferToImage(GpuImage &image, VkBuffer staging, VkDeviceSize offset,
                         uint32_t width, uint32_t height,
                         VkImageAspectFlags aspect);
  bool uploadColorTo(const DecodedVideoFrame &frame, GpuImage &image);
  bool dispatchYuvConvert(const DecodedVideoFrame &frame, bool compareEnabled,
                          float sharpness);
  bool dispatchBicubicPrefilter(const DecodedVideoFrame &frame);
  bool uploadMotionCpu(const std::vector<MvEntry> &mvs);
  bool importDrmPrimeFrame(const DecodedVideoFrame &frame,
                           ImportedDrmFrame &imported);
  bool importAndConvertDrmFrame(const DecodedVideoFrame &frame);
  bool createImportedDrmRuntime(const ImportedDrmFrame &frame,
                                ImportedDrmRuntime &rt);
  void destroyImportedDrmRuntime(ImportedDrmRuntime &rt);
  // drmFrameMatchesLinearNv12 + drmFourccToVkFormat moved to
  // render/upload/DrmFormat.hpp as free functions (they have no member state).

  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = ~0u;
  uint32_t presentationQueueFamily_ = ~0u;

  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  // Decoded input images remain at source dimensions. The model image is
  // produced by the GPU bicubic prefilter at modelW_ x modelH_.
  GpuImage yPlane_, uPlane_, vPlane_;
  GpuImage sourceModel_, color_, rawPresentation_, motion_, depth_, reactive_, tcMask_,
      exposure_;
  // Output images (output dims)
  GpuImage output_, history_[2], recurrent_[2], reprojectedColor_;
  GpuImage presentation_;
  // Keep one recently used presentation target so a resize oscillating between
  // two window sizes reuses its VkImage/allocation instead of reallocating on
  // every frame. The active image is swapped with this cache only after the
  // synchronous upload/dispatch fence has retired.
  GpuImage presentationRetained_;
  std::atomic<uint32_t> historyIndex_{0};

  // Host-visible staging buffer (reused across uploads; grown as needed).
  GpuBuffer staging_;
  void *stagingMapped_ = nullptr;
  SwsContext *swsColor_ = nullptr;
  int swsW_ = 0;
  int swsH_ = 0;
  int swsFormat_ = -1;
  std::vector<uint8_t> yuv420Scratch_;
  std::vector<uint8_t> rgbaScratch_;
  std::vector<uint8_t> rgbaSharpScratch_;
  std::vector<float> lumaScratch_;
  VkDescriptorSetLayout yuvDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool yuvDescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout yuvPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline yuvPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout drmDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool drmDescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout drmPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline drmPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout motionDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool motionDescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout motionPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline motionPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout prefilterDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool prefilterDescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout prefilterPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline prefilterPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet prefilterSet_ = VK_NULL_HANDLE;

  // EASU 2x pre-scale pipeline + intermediate image.
  GpuImage easuImage_;
  VkDescriptorSetLayout easuDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool easuDescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout easuPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline easuPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet easuSet_ = VK_NULL_HANDLE;
  bool easuActive_ = false;
  std::atomic<float> sharpness_{0.3f};
  std::atomic<int> presentationScaler_{2};
  std::atomic<bool> compareEnabled_{false};
  ImportedDrmRuntime drmRt_{};

  GpuBuffer motionVectors_;
  GpuBuffer motionOwners_;
  void *motionVectorsMapped_ = nullptr;

  uint32_t srcW_ = 0, srcH_ = 0;
  uint32_t modelW_ = 0, modelH_ = 0;
  uint32_t outW_ = 0, outH_ = 0;
  bool frameUploadBatch_ = false;
  bool deferredFrameUploads_ = false;
  bool resetYuvDescriptorsAfterBatch_ = false;
  bool resetDrmDescriptorsAfterBatch_ = false;
  bool resetMotionDescriptorsAfterBatch_ = false;
};

} // namespace temporal_forge
