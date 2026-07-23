// YuvConstants.cpp — YUV->RGBA push-constants builder.
#include "render/upload/YuvConstants.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace temporal_forge {

YuvPushConstants yuvPushConstants(const DecodedVideoFrame &frame,
                                  bool compareEnabled, float sharpness) {
  float kr = 0.2126f;
  float kb = 0.0722f;
  switch (static_cast<AVColorSpace>(frame.colorSpace)) {
  case AVCOL_SPC_BT2020_NCL:
  case AVCOL_SPC_BT2020_CL:
    kr = 0.2627f;
    kb = 0.0593f;
    break;
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M:
    kr = 0.2990f;
    kb = 0.1140f;
    break;
  case AVCOL_SPC_FCC:
    kr = 0.3000f;
    kb = 0.1100f;
    break;
  case AVCOL_SPC_SMPTE240M:
    kr = 0.2120f;
    kb = 0.0870f;
    break;
  case AVCOL_SPC_RGB:
  case AVCOL_SPC_BT709:
  case AVCOL_SPC_UNSPECIFIED:
  default:
    // Follow FFmpeg's common SD fallback when metadata is missing.
    if (frame.colorSpace == AVCOL_SPC_UNSPECIFIED && frame.height < 720) {
      kr = 0.2990f;
      kb = 0.1140f;
    }
    break;
  }

  const bool fullRange = frame.colorRange == AVCOL_RANGE_JPEG;
  const float yOffset = fullRange ? 0.0f : 16.0f / 255.0f;
  const float yScale = fullRange ? 1.0f : 255.0f / 219.0f;
  const float chromaScale = fullRange ? 1.0f : 255.0f / 224.0f;
  const float kg = 1.0f - kr - kb;

  return {static_cast<float>(frame.width),
          static_cast<float>(frame.height),
          sharpness,
          compareEnabled ? 1.0f : 0.0f,
          yOffset,
          yScale,
          128.0f / 255.0f,
          chromaScale,
          2.0f * (1.0f - kr) * chromaScale,
          -2.0f * kb * (1.0f - kb) / kg * chromaScale,
          -2.0f * kr * (1.0f - kr) / kg * chromaScale,
          2.0f * (1.0f - kb) * chromaScale};
}

} // namespace temporal_forge
