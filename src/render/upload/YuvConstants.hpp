// YuvConstants.hpp — YUV->RGBA conversion push-constants builder.
//
// Extracted from GpuImageUploader.cpp. Pure (no device state): given a decoded
// frame + settings, computes the shader push-constant block (color matrix
// coefficients, range offsets/scales) the YUV conversion compute shader reads.
#pragma once
#include "media/VideoDecoder.hpp" // for DecodedVideoFrame

#include <cstdint>

namespace temporal_forge {

// Push-constant block consumed by yuv_to_fsr_input.comp / drm_yuv_to_fsr_input.comp.
// Layout must stay aligned (16-byte) and match the shader's std430 push constant.
struct alignas(16) YuvPushConstants {
    float width, height, sharpness, compareEnabled;
    float yOffset, yScale, chromaOffset, chromaScale;
    float rV, gU, gV, bU;
    float chromaPhaseX, chromaPhaseY, sourceBitDepth, reserved;
    // Synthetic video jitter in render/source pixels. Zero means the decoded
    // sample is unshifted; non-zero values are applied by the same conversion
    // pass that constructs the FSR color input.
    float jitterX, jitterY, jitterEnabled, jitterReserved;
};

// yuvPushConstants: build the conversion push-constants for a frame.
//
// Called by: GpuImageUploader::dispatchYuvConvert and ::importAndConvertDrmFrame.
// Notes:     Picks Kr/Kb from the frame's AVColorSpace (Rec.709 default,
//            Rec.2020/BT.470BG/FCC/240M variants, with an SD fallback for
//            unspecified SD content), and derives the full/limited range
//            offsets + the YUV->RGB matrix coefficients from them.
YuvPushConstants yuvPushConstants(const DecodedVideoFrame &frame,
                                  bool compareEnabled, float sharpness,
                                  float jitterX = 0.0f, float jitterY = 0.0f,
                                  bool jitterEnabled = false);

} // namespace temporal_forge
