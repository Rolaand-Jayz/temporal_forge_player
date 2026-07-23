// Fsr4ConvSteps.hpp — the 39-step FSR4 convolution table + config builder.
//
// Extracted from Fsr4DispatchHarness.cpp to keep the harness .cpp focused on
// Vulkan resource lifecycle. These are PURE free functions/data (no GPU/device
// coupling) so they are safe to live in their own translation unit.
//
// RE PROVENANCE: the kFsr4ConvSteps offsets are reverse-engineered from the
// extracted FSR4 weight blob (see docs/FSR4_RE_STATUS.md). They are NOT
// arbitrary — they are the exact byte offsets the RE-derived loader expects.
// Do not renumber without re-deriving from the blob.
#pragma once
#include "render/Fsr4DispatchHarness.hpp" // for Fsr4DispatchHarness::ConvPassConfig

#include <array>
#include <cstddef>
#include <cstdint>

namespace temporal_forge {

// Description of a single convolution step in the 39-step FSR4 chain.
// Fields mirror the recovered v4.1 tensor-map.json entries.
struct Fsr4ConvStep {
    uint32_t weightOff;   // byte offset into the weight blob
    uint32_t biasOff;     // byte offset into the FP16 bias zone
    int cin;              // logical input channels
    int cout;             // output channels
    int kw, kh;           // kernel dimensions
    bool depthwise;       // true = depthwise (3x3/2x2), false = pointwise (1x1)
    uint32_t spatialDiv;  // U-pyramid level (1=full, 2=half, 4, 8)
    bool fp16;            // pass 0 uses FP16 weights; the rest are FP8
    bool isScale;         // true = stride-2 downscale/upscale (2x2 kernel)
};

// The full 39-entry convolution step table. The conv dispatch iterates this
// to record each pass in order. constexpr so it lives in the header.
constexpr std::array<Fsr4ConvStep, 39> kFsr4ConvSteps{{
    // Pass 0 has FP16 weights at offset 0. The remaining convolution weights
    // are FP8 bytes in the 7208..130087 zone; all 39 bias vectors are FP16 in
    // the 0..7207 zone.
    {0, 1024, 7, 16, 2, 2, true, 2, true, true},
    {7208, 1088, 16, 16, 3, 3, true, 2, false, false},
    {9512, 1152, 16, 32, 1, 1, false, 2, false, false},
    {10024, 1280, 32, 16, 1, 1, false, 2, false, false},
    {10536, 1344, 16, 16, 3, 3, true, 2, false, false},
    {12840, 1408, 16, 32, 1, 1, false, 2, false, false},
    {13352, 1536, 32, 16, 1, 1, false, 2, false, false},
    {13864, 1600, 16, 32, 2, 2, true, 4, false, true},
    {15912, 1728, 16, 16, 3, 3, true, 4, false, false},
    {18216, 1792, 32, 64, 1, 1, false, 4, false, false},
    {20264, 2048, 64, 32, 1, 1, false, 4, false, false},
    {22312, 2176, 16, 16, 3, 3, true, 4, false, false},
    {24616, 2240, 32, 64, 1, 1, false, 4, false, false},
    {26664, 2496, 64, 32, 1, 1, false, 4, false, false},
    {28712, 2624, 32, 64, 2, 2, true, 8, false, true},
    {36904, 2880, 16, 32, 3, 3, true, 8, false, false},
    {41512, 3008, 64, 128, 1, 1, false, 8, false, false},
    {49704, 3520, 128, 64, 1, 1, false, 8, false, false},
    {57896, 3776, 16, 32, 3, 3, true, 8, false, false},
    {62504, 3904, 64, 128, 1, 1, false, 8, false, false},
    {70696, 4416, 128, 64, 1, 1, false, 8, false, false},
    {78888, 4672, 16, 32, 3, 3, true, 8, false, false},
    {83496, 4800, 64, 128, 1, 1, false, 8, false, false},
    {91688, 5312, 128, 64, 1, 1, false, 8, false, false},
    {119336, 5568, 64, 32, 2, 2, true, 4, false, true},
    {99880, 5696, 16, 16, 3, 3, true, 4, false, false},
    {102184, 5760, 32, 64, 1, 1, false, 4, false, false},
    {104232, 6016, 64, 32, 1, 1, false, 4, false, false},
    {106280, 6144, 16, 16, 3, 3, true, 4, false, false},
    {108584, 6208, 32, 64, 1, 1, false, 4, false, false},
    {110632, 6464, 64, 32, 1, 1, false, 4, false, false},
    {127528, 6592, 32, 16, 2, 2, true, 2, false, true},
    {112680, 6656, 16, 16, 3, 3, true, 2, false, false},
    {114984, 6720, 16, 32, 1, 1, false, 2, false, false},
    {115496, 6848, 32, 16, 1, 1, false, 2, false, false},
    {116008, 6912, 16, 16, 3, 3, true, 2, false, false},
    {118312, 6976, 16, 32, 1, 1, false, 2, false, false},
    {118824, 7104, 32, 16, 1, 1, false, 2, false, false},
    {129576, 7168, 16, 8, 2, 2, true, 1, false, true},
}};

// makeConvConfig: translate an Fsr4ConvStep (plus its successor) into the
//                 Fsr4DispatchHarness::ConvPassConfig used to record a pass.
//
// Called by: Fsr4DispatchHarness::dispatchFrame and ::recordNativeInt8Graph.
// Notes:     Handles the encoder's 7->8 NHWC lane padding and the recovered
//            ConvNext block semantics (ReLU only after the expanding pointwise).
Fsr4DispatchHarness::ConvPassConfig makeConvConfig(const Fsr4ConvStep &s,
                                                   const Fsr4ConvStep *next);

// isResidualBlockStart / isResidualBlockEnd: classify a step index into the
// recovered residual-block boundaries (the conv chain is a sequence of residual
// blocks). Called by: Fsr4DispatchHarness::dispatchFrame and
// ::recordNativeInt8Graph to insert the residual-add/scatter ops.
bool isResidualBlockStart(size_t step);
bool isResidualBlockEnd(size_t step);

} // namespace temporal_forge
