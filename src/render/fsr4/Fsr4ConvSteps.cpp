// Fsr4ConvSteps.cpp — definitions for the FSR4 conv-step config builder.
// (The constexpr kFsr4ConvSteps table lives in the header.)
#include "render/fsr4/Fsr4ConvSteps.hpp"

namespace temporal_forge {

Fsr4DispatchHarness::ConvPassConfig makeConvConfig(const Fsr4ConvStep &s,
                                                   const Fsr4ConvStep *next) {
  Fsr4DispatchHarness::ConvPassConfig cfg{};
  cfg.weightOffset = s.weightOff;
  cfg.biasOffset = s.biasOff;
  cfg.channelsIn = s.cin;
  cfg.channelsOut = s.cout;
  cfg.kernelW = s.kw;
  cfg.kernelH = s.kh;
  // Encoder input is logically 7 channels but physically stored as 8-lane
  // NHWC. The convolution consumes seven weights while advancing each pixel
  // by eight floats.
  if (s.fp16 && s.cin == 7) {
    cfg.weightChannelsIn = 7;
    cfg.channelsIn = 8;
  }
  if (s.depthwise && !s.isScale && next && !next->depthwise &&
      next->spatialDiv == s.spatialDiv && next->cin > s.cout) {
    cfg.weightChannelsIn = s.cin;
    cfg.weightChannelsOut = s.cout;
    cfg.channelsIn = next->cin;
    cfg.channelsOut = next->cin;
  }
  cfg.depthwise = s.depthwise;
  // The recovered ConvNext block applies ReLU only after the expanding
  // pointwise convolution. The depthwise and contracting convolutions,
  // including the final transpose convolution, remain signed.
  cfg.hasRelu = !s.depthwise && s.cout > s.cin;
  cfg.spatialScale = s.spatialDiv;
  cfg.fp16Weights = s.fp16;
  // Biases occupy the FP16 zone at offsets 1024..7207. They are stored as
  // two-byte values even though the RE tensor names call them Tensor1f.
  cfg.fp16Bias = true;
  cfg.isScale = s.isScale;
  // Every channel-reducing 2x2 scale operator is a stride-2 transpose
  // convolution. Decoder stages 24 and 31 were previously misclassified as
  // downscales merely because they were not yet at spatialDiv==1.
  cfg.isUpscale = s.isScale && s.cout < s.cin;
  // The final decoder transpose-convolution feeds the postpass as an
  // FP16 tensor. All earlier network boundaries are FP8 CopySat tensors.
  cfg.outputFp16 = s.isScale && s.spatialDiv == 1;
  return cfg;
}

bool isResidualBlockStart(size_t step) {
  return step == 1 || step == 4 || step == 8 || step == 11 || step == 15 ||
         step == 18 || step == 21 || step == 25 || step == 28 || step == 32 ||
         step == 35;
}

bool isResidualBlockEnd(size_t step) {
  return step == 3 || step == 6 || step == 10 || step == 13 || step == 17 ||
         step == 20 || step == 23 || step == 27 || step == 30 || step == 34 ||
         step == 37;
}

} // namespace temporal_forge
