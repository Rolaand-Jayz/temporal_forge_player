// Fsr4DispatchHarness.hpp — owns the Vulkan compute pipeline that runs the
// FSR 4.1 RE neural upscaler on RDNA3.
//
// Source: local RE notes, recovered tensor metadata, and the local Quality
// weight blob. This harness is the executable backend.
//
// The harness builds the device resources, the descriptor-set layout for the
// recovered universal binding map, and one compute pipeline per pass. The
// Fsr4Int8Backend creates a harness, uploads the weight blob once, then calls
// dispatchFrame() per source frame.
//
// Dispatch order:
//   1. prepass           — video EOTF + feature/recurrent-state extraction
//   2. 39 conv steps     — the neural network, INT8 cooperative-matrix DOT4
//   3. postpass          — composite + sigmoid temporal blend
//   4. SPD               — exposure/luma mip
//
// Current shaders use (32,1,1) thread groups based on the RE notes.
#pragma once
#include "backend/GpuCapabilityProbe.hpp"
#include "backend/UpscaleTypes.hpp"
#include "backend/WeightBlob.hpp"
#include "config/QualityLabConfig.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace temporal_forge {

struct Fsr4DispatchResources {
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint32_t outputWidth = 0;
  uint32_t outputHeight = 0;
  // The user-selected multiplier. Zero preserves the standalone harness
  // compatibility path, which infers a tier from output/source dimensions.
  float requestedScale = 0.0f;
  // Recovered universal buffers (spec §3.1):
  //   weight blob (SRV 0:18), shared scratch (UAV 0:11),
  //   accumulation (UAV 1:0), feature/history (UAV 2:1), prepass out (UAV 2:3)
  VkBuffer weightBuffer = VK_NULL_HANDLE;
  VkDeviceMemory weightMemory = VK_NULL_HANDLE;
  VkBuffer sharedScratch = VK_NULL_HANDLE;
  VkDeviceMemory sharedScratchMemory = VK_NULL_HANDLE;
  VkBuffer accumBuffer = VK_NULL_HANDLE;
  VkDeviceMemory accumMemory = VK_NULL_HANDLE;
  VkBuffer featureBuffer = VK_NULL_HANDLE;
  VkDeviceMemory featureMemory = VK_NULL_HANDLE;
  VkBuffer finalTensorBuffer = VK_NULL_HANDLE;
  VkDeviceMemory finalTensorMemory = VK_NULL_HANDLE;
  // Input/output textures bound to the recovered SRV/UAV color slots.
  VkImageView colorInputView = VK_NULL_HANDLE;
  VkImageView colorOutputView = VK_NULL_HANDLE;
};

// Result of a single frame dispatch.
struct Fsr4DispatchResult {
  bool ok = false;
  UpscaleError error = UpscaleError::None;
  double dispatchMs = 0.0;
  double gpuMs = 0.0;
  std::string failReason;
};

// Real-frame dispatch input: the source images, display history, recurrent
// state, and output images. All views are in GENERAL layout and must remain
// valid for the duration of dispatchFrame().
struct FrameDispatchInput {
  // Optional upload/conversion work recorded on the same queue family. It is
  // submitted immediately before the FSR command buffer under the same fence.
  VkCommandBuffer prefixCommandBuffer = VK_NULL_HANDLE;
  VkImageView colorView = VK_NULL_HANDLE;    // rgb10_a2, source dims
  VkImage colorImage = VK_NULL_HANDLE;       // matching image for barriers
  VkImageView motionView = VK_NULL_HANDLE;   // rg16f,   source dims
  VkImageView depthView = VK_NULL_HANDLE;    // r32f,    source dims
  VkImageView reactiveView = VK_NULL_HANDLE; // r8,      source dims
  VkImageView tcMaskView = VK_NULL_HANDLE;   // r8,      source dims
  VkImageView exposureView = VK_NULL_HANDLE; // r16f,    1x1
  VkImageView outputView = VK_NULL_HANDLE;   // rgba8, output dims
  VkImageView historyReadView = VK_NULL_HANDLE;
  VkImageView historyWriteView = VK_NULL_HANDLE;
  VkImageView recurrentReadView = VK_NULL_HANDLE;  // rgba16f, output dims
  VkImageView recurrentWriteView = VK_NULL_HANDLE; // rgba16f, output dims
  VkImage outputImage = VK_NULL_HANDLE; // raw image for layout transitions
  VkImage historyReadImage = VK_NULL_HANDLE;
  VkImage historyWriteImage = VK_NULL_HANDLE;
  VkImage recurrentReadImage = VK_NULL_HANDLE;
  VkImage recurrentWriteImage = VK_NULL_HANDLE;
  float jitterX = 0.0f;
  float jitterY = 0.0f;
  float frameTimeMs = 16.6667f;
  float historyConfidence = 1.0f;
  bool reset = false;
  bool hdr = false; // false = SDR (Rec.709 EOTF in prepass)
  // 0 = SDR/sRGB, 1 = PQ, 2 = HLG. The postpass keeps SDR output by
  // default; TFORGE_FSR4_HDR_OUTPUT opts into encoding the selected transfer.
  uint32_t transfer = 0;
};

class Fsr4DispatchHarness {
public:
  Fsr4DispatchHarness();
  ~Fsr4DispatchHarness();
  Fsr4DispatchHarness(const Fsr4DispatchHarness &) = delete;
  Fsr4DispatchHarness &operator=(const Fsr4DispatchHarness &) = delete;

  // Initialize the harness on a device + capability. Returns false on
  // failure (caller should fail closed to FSR 3.1.5).
  bool init(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
            uint32_t queueFamily, const GpuCapability &cap);

  // Allocate per-source/preset resources. Called when the source dims,
  // preset, or output dims change (NOT on window resize — spec 02/08).
  bool allocateResources(const Fsr4DispatchResources &r);

  // Resolve all quality-lab values once before dispatch. The default disabled
  // config leaves the recovered current postpass path unchanged.
  void setQualityLabConfig(const QualityLabConfig &config) {
    qualityLabConfig_ = config;
  }

  // Upload the weight blob to GPU memory (one-shot per backend create).
  bool uploadWeights(const Fsr4BlobView &blob);

  // Seed the scratch buffer with synthetic input data for proof validation.
  // Fills the feature region (0x52xxxxxx) with a known non-zero pattern so
  // the proof runner can verify the conv chain propagates data through to
  // the output buffers. Returns false if the scratch buffer isn't mappable.
  bool seedSyntheticInput();

  // Run one synthetic proof frame through the conv-chain. reset = true
  // invalidates temporal history (seek/scene-cut/new-file). [Legacy
  // synthetic-seed path — used by the proof runner. Real playback uses the
  // FrameDispatchInput overload.]
  Fsr4DispatchResult dispatchFrame(bool reset);

  // Real-frame dispatch: runs the prepass + conv chain + postpass +
  // SPD pipeline on the provided input images, writing to outputView. This
  // closes gaps #4 (prepass never dispatched) and #5 (output images unbound).
  Fsr4DispatchResult dispatchFrame(const FrameDispatchInput &in);

  // Submit a real frame without blocking the caller on the GPU fence. The
  // caller must keep every image/buffer referenced by `in` alive until
  // waitForFrame() returns successfully, then call waitForFrame() before
  // reusing this harness or its frame resources. This is the safe boundary
  // used by the in-flight slot integration; it is deliberately separate from
  // dispatchFrame() so existing synchronous callers retain their semantics.
  Fsr4DispatchResult dispatchFrameAsync(const FrameDispatchInput &in);
  Fsr4DispatchResult waitForFrame();
  [[nodiscard]] bool frameInFlight() const { return frameInFlight_; }

  // Seed the scratch buffer directly with color data, bypassing the prepass.
  // Writes the uploaded color image's RGBA values into the scratch FP16
  // feature region at the layout conv_dw expects (channels × W × H). Used as
  // a diagnostic when the prepass output layout doesn't match the conv input.
  bool seedColorToScratch(VkImageView colorView);

  // Drive one conv pass + its scatter. The pass config holds the weight
  // offset + channel dims derived from the tensor map. This is the core
  // of the conv sequence.
  struct ConvPassConfig {
    uint32_t weightOffset = 0; // physical v4.1 tensor offset in the blob
    uint32_t biasOffset = 0;   // byte offset into the model bias region
    int channelsIn = 0;
    int channelsOut = 0;
    int kernelW = 3, kernelH = 3;
    int weightChannelsIn = 0;
    int weightChannelsOut = 0;
    bool depthwise = true; // false = pointwise (1x1, no ReLU)
    bool hasRelu = true;
    uint32_t spatialScale = 0; // U-pyramid level (0=1x,1=0.5x,...)
    bool fp16Weights =
        false;               // pass0 downscale uses FP16 weights (the only one)
    bool fp16Bias = false;   // recovered 4.1.0 bias zone stores FP16 values
    bool isScale = false;    // true = stride-2 downscale/upscale (2×2 kernel)
    bool isUpscale = false;  // final 2×2 transpose convolution
    bool outputFp16 = false; // final decoder output is FP16, not FP8
  };
  void recordConvPass(VkCommandBuffer cmd, const ConvPassConfig &cfg,
                      uint32_t dispatchW, uint32_t dispatchH);

  void destroy();

  [[nodiscard]] bool initialized() const { return device_ != VK_NULL_HANDLE; }
  // Native fixed-shape INT8 graphs carry their own initializer and do not
  // require the legacy RE weight blob used by the generic fallback.
  [[nodiscard]] bool usesNativeInt8() const { return nativeInt8Active_; }
  [[nodiscard]] const GpuCapability &capability() const { return cap_; }
  [[nodiscard]] const Fsr4DispatchResources &resources() const { return res_; }

  // DIAGNOSTIC: runs only the prepass in a standalone submit, then reads
  // back the first N floats of scratch (where the prepass writes features).
  // Returns true if non-zero features were found. Used to isolate where the
  // conv chain loses data. Set diagnoseEnabled_ = true to activate.
  bool diagnosePrepass(const FrameDispatchInput &in);

  // Byte offset of the final decoder tensor within finalTensorBuffer. The
  // final transpose convolution writes through the dedicated edge binding
  // at offset zero; intermediate tensors remain in scratch ping-pong slots.
  // Returns 0 until dispatchFrame() has run at least once.
  [[nodiscard]] VkDeviceSize finalAccumOffsetBytes() const {
    return finalAccumOffsetBytes_;
  }
  [[nodiscard]] size_t finalAccumFloatCount() const {
    return finalAccumFloatCount_;
  }

  // Copies the final decoder tensor through a temporary host-visible buffer
  // for experimental quality/proof inspection. Working tensors stay VRAM.
  bool readbackFinalAccum(std::vector<float> &out);

  bool downscaleRgb10(VkImage sourceImage, VkImageView sourceView,
                      uint32_t sourceWidth, uint32_t sourceHeight,
                      VkImage destinationImage, VkImageView destinationView,
                      uint32_t destinationWidth, uint32_t destinationHeight);

private:
  enum class NativeInt8Graph : uint8_t {
    None,
    Quality1080,
    Quality2160,
    UltraPerformance1080,
    UltraPerformance2160,
    Performance2160,
    Performance4320,
    QualityFourThree1440,
    UltraPerformanceFourThree1440,
    PerformanceFourThree1440,
    QualityFourThree2880,
    UltraPerformanceFourThree2880,
    PerformanceFourThree2880,
  };
  bool createDescriptorLayout();
  bool createPrepassPipeline();
  bool createConvPipelines(); // depthwise + pointwise conv
  bool createScatterPipeline();
  bool createPostpassPipeline();
  bool createSpdPipeline();
  bool createDownscalePipeline();
  bool createNativeInt8Pipelines();
  bool ensureNativeInt8Pipelines(NativeInt8Graph graph);
  void persistGenericPipelineCache();
  void persistNativeInt8PipelineCache();
  bool createCommandBuffer();
  bool prepareNativeInt8Resources();
  void recordNativeInt8Graph(VkCommandBuffer cmd);
  // Record the prepass dispatch (prepassPipeline_) into cmd, binding the
  // 6 input images + weight blob + prepass-output SSBO. Closes gap #4.
  void recordPrepass(VkCommandBuffer cmd, const FrameDispatchInput &in);
  void recordResidualCapture(VkCommandBuffer cmd, uint32_t scratchSlot,
                             uint32_t elementCount,
                             uint32_t featureBaseWords = 0);
  void recordResidualAdd(VkCommandBuffer cmd, uint32_t scratchSlot,
                         uint32_t elementCount, uint32_t featureBaseWords = 0);
  void recordFusedResidualBlock16(VkCommandBuffer cmd, uint32_t dispatchW,
                                  uint32_t dispatchH, uint32_t spatialWeight,
                                  uint32_t spatialBias, uint32_t expandWeight,
                                  uint32_t expandBias, uint32_t contractWeight,
                                  uint32_t contractBias);
  // GPU buffer helpers (used by allocateResources + uploadWeights).
  bool createGpuBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags memProps, VkBuffer &buf,
                       VkDeviceMemory &mem, const char *tag);
  void freeGpuBuffer(VkBuffer &buf, VkDeviceMemory &mem);

  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = ~0u;
  GpuCapability cap_;

  VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout convDescLayout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout postpassDescLayout_ =
      VK_NULL_HANDLE; // postpass has different binding types
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkPipelineLayout prepassLayout_ = VK_NULL_HANDLE;
  VkPipeline prepassPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout downscaleDescLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout downscaleLayout_ = VK_NULL_HANDLE;
  VkPipeline downscalePipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout convLayout_ = VK_NULL_HANDLE; // shared by dw + pw
  VkPipeline convDwPipeline_ = VK_NULL_HANDLE;   // depthwise (3x3/4x4/5x4)
  VkPipeline convPwPipeline_ = VK_NULL_HANDLE;   // pointwise (1x1, no ReLU)
  VkPipeline convPwCoopPipeline_ =
      VK_NULL_HANDLE; // 16x16x16 SINT8 cooperative matrix
  VkPipeline convFp16CoopPipeline_ =
      VK_NULL_HANDLE; // 16x16x16 FP16 cooperative fallback
  VkPipeline convPwFp16DirectPipeline_ =
      VK_NULL_HANDLE; // direct-storage 1x1 FP16 cooperative path
  VkPipeline convSpatialFp16DirectPipeline_ =
      VK_NULL_HANDLE; // direct-load full 16->16 3x3 path
  VkPipeline residualBlock16Int8Pipeline_ =
      VK_NULL_HANDLE; // fused INT8 16ch residual block
  VkPipeline convUpscaleFp16CoopPipeline_ =
      VK_NULL_HANDLE; // cooperative FP16 2x2 transpose path
  VkPipeline convUpscaleFp16ScalarFinalPipeline_ =
      VK_NULL_HANDLE; // final 16->8 pixel-owned path
  VkPipeline convDownscaleFp16CoopPipeline_ =
      VK_NULL_HANDLE; // cooperative FP16 7->16 2x2 path
  VkPipeline convDownscaleFp16DirectPipeline_ =
      VK_NULL_HANDLE; // direct FP16 16->32/32->64 stride-2 path
  VkPipeline residualAddPipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout scatterLayout_ = VK_NULL_HANDLE;
  VkPipeline scatterPipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout postpassLayout_ = VK_NULL_HANDLE;
  VkPipeline postpassPipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout spdLayout_ = VK_NULL_HANDLE;
  VkPipeline spdPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout nativeInt8DescLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool nativeInt8DescPool_ = VK_NULL_HANDLE;
  VkPipelineLayout nativeInt8Layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 14> nativeInt8QualityPipelines1080_{};
  std::array<VkPipeline, 14> nativeInt8QualityPipelines2160_{};
  std::array<VkPipeline, 14> nativeInt8UltraPipelines1080_{};
  std::array<VkPipeline, 14> nativeInt8UltraPipelines2160_{};
  std::array<VkPipeline, 14> nativeInt8PerformancePipelines2160_{};
  std::array<VkPipeline, 14> nativeInt8PerformancePipelines4320_{};
  std::array<VkPipeline, 14> nativeInt8QualityFourThreePipelines1440_{};
  std::array<VkPipeline, 14> nativeInt8UltraFourThreePipelines1440_{};
  std::array<VkPipeline, 14> nativeInt8PerformanceFourThreePipelines1440_{};
  std::array<VkPipeline, 14> nativeInt8QualityFourThreePipelines2880_{};
  std::array<VkPipeline, 14> nativeInt8UltraFourThreePipelines2880_{};
  std::array<VkPipeline, 14> nativeInt8PerformanceFourThreePipelines2880_{};
  VkDescriptorSet nativeInt8Set_ = VK_NULL_HANDLE;
  VkPipelineCache genericPipelineCache_ = VK_NULL_HANDLE;
  VkPipelineCache nativeInt8PipelineCache_ = VK_NULL_HANDLE;
  VkBuffer nativeInt8Initializer_ = VK_NULL_HANDLE;
  VkDeviceMemory nativeInt8InitializerMemory_ = VK_NULL_HANDLE;
  VkBuffer nativeInt8Output_ = VK_NULL_HANDLE;
  VkDeviceMemory nativeInt8OutputMemory_ = VK_NULL_HANDLE;
  bool nativeInt8PipelinesAvailable_ = false;
  bool nativeInt8QualityPipelines1080Available_ = false;
  bool nativeInt8QualityPipelines2160Available_ = false;
  bool nativeInt8UltraPipelines1080Available_ = false;
  bool nativeInt8UltraPipelines2160Available_ = false;
  bool nativeInt8PerformancePipelines2160Available_ = false;
  bool nativeInt8PerformancePipelines4320Available_ = false;
  bool nativeInt8QualityFourThreePipelines1440Available_ = false;
  bool nativeInt8UltraFourThreePipelines1440Available_ = false;
  bool nativeInt8PerformanceFourThreePipelines1440Available_ = false;
  bool nativeInt8QualityFourThreePipelines2880Available_ = false;
  bool nativeInt8UltraFourThreePipelines2880Available_ = false;
  bool nativeInt8PerformanceFourThreePipelines2880Available_ = false;
  bool nativeInt8Active_ = false;
  NativeInt8Graph nativeInt8Graph_ = NativeInt8Graph::None;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkCommandBuffer nativeInt8Cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool frameInFlight_ = false;
  bool asyncDispatchRequested_ = false;
  std::chrono::steady_clock::time_point pendingDispatchStart_{};
  Fsr4DispatchResult pendingDispatchResult_{};
  VkQueryPool timestampPool_ = VK_NULL_HANDLE;
  float timestampPeriodNs_ = 0.0f;
  static constexpr uint32_t kTimestampQueryCount =
      42; // start, prepass, 39 ops, postpass

  static constexpr uint32_t kMaxPasses = 64;
  // Fourteen neural passes plus the postpass use the same mapped ring. The
  // postpass Quality Lab constants occupy eight vec4 slots; keep every slot
  // uniformly aligned so descriptor offsets remain valid on Vulkan devices.
  static constexpr uint32_t kCbSize = 128;

  Fsr4DispatchResources res_;
  VkDescriptorSet prepassSet_ = VK_NULL_HANDLE;
  // These bindings are stable for a target allocation. Reuse the descriptor
  // objects across frames; only the per-frame uniform contents and temporal
  // image bindings are rewritten while recording.
  std::array<VkDescriptorSet, kMaxPasses> convDescriptorSets_{};
  std::array<VkDescriptorSet, kMaxPasses> residualDescriptorSets_{};
  VkDescriptorSet postpassSet_ = VK_NULL_HANDLE;

  bool weightsUploaded_ = false;
  uint32_t passCounter_ = 0;
  uint32_t residualOpCounter_ = 0;
  // Ping-pong slot size (FP16 words) and the byte offset where the last
  // conv step wrote its output. Set at the end of the conv chain in
  // dispatchFrame(); read by PlaybackEngine's direct-accum readback.
  uint32_t slotSizeWords_ = 0;
  VkDeviceSize finalAccumOffsetBytes_ = 0;
  size_t finalAccumFloatCount_ = 0;
  bool finalOutputInScratch_ = false;
  bool diagnoseEnabled_ = false;
  QualityLabConfig qualityLabConfig_{};
  // Pre-allocated uniform buffer for all pass constants (avoids per-pass
  // vkAllocateMemory/vkFreeMemory which dominate dispatch latency).
  VkBuffer cbRingBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory cbRingMemory_ = VK_NULL_HANDLE;
  void *cbRingMapped_ = nullptr;
  // padded uniform slot
};

} // namespace temporal_forge
