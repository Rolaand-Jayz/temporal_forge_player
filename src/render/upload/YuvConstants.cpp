// YuvConstants.cpp — YUV->RGBA push-constants builder.
#include "render/upload/YuvConstants.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace temporal_forge {

// The final push-constant component is already reserved by both YUV upload
// shaders. Keep the experiment opt-in and encode the flag as float bits so the
// existing 16-byte push-constant layout does not change.
// These flags share the existing reserved push-constant word. Bit zero keeps
// the original Rec.709 experiment; the remaining bits select only conversion
// policies that the two upload shaders already have enough information to run.
constexpr uint32_t YUV_FLAG_REC709_INPUT_EOTF = 1u;
constexpr uint32_t YUV_FLAG_LINEAR_INPUT_EOTF = 2u;
constexpr uint32_t YUV_FLAG_CHROMA_BILINEAR = 4u;
constexpr uint32_t YUV_FLAG_PRE_CAS = 8u;

YuvPushConstants yuvPushConstants(const DecodedVideoFrame &frame,
                                  bool compareEnabled, float sharpness) {
  // Keep the historical SD fallback unless this explicitly requested
  // experiment is enabled. The override is intentionally consumed here,
  // before coefficient construction, so it can affect only unspecified matrix
  // metadata and cannot alter explicit BT.601/BT.709/BT.2020 selections.
  const bool unknownMatrixBt709 =
      std::getenv("TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709") != nullptr;

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
  case AVCOL_SPC_BT709:
  case AVCOL_SPC_RGB:
    break;
  case AVCOL_SPC_UNSPECIFIED:
  default:
    // Follow FFmpeg's common SD fallback when metadata is missing.
    if (frame.height < 720 &&
        (!unknownMatrixBt709 ||
         frame.colorSpace != AVCOL_SPC_UNSPECIFIED)) {
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
  // FFmpeg's chroma location describes the 4:2:0 sample's phase relative to
  // the luma grid. CENTER preserves the existing centered reconstruction;
  // LEFT/TOP variants keep edge-aligned sources from shifting half a texel.
  float chromaPhaseX = 0.5f;
  float chromaPhaseY = 0.5f;
  if (frame.chromaLocation == AVCHROMA_LOC_LEFT)
    chromaPhaseX = 0.25f;
  else if (frame.chromaLocation == AVCHROMA_LOC_TOPLEFT)
    chromaPhaseX = chromaPhaseY = 0.25f;
  else if (frame.chromaLocation == AVCHROMA_LOC_TOP)
    chromaPhaseY = 0.25f;

  // Metadata remains the default phase. Review captures may choose one of
  // the already-supported phase conventions explicitly without changing the
  // decoder's metadata or the normal playback path.
  if (const char *phase = std::getenv("TFORGE_FSR4_CHROMA_PHASE")) {
    if (std::strcmp(phase, "center") == 0) {
      chromaPhaseX = chromaPhaseY = 0.5f;
    } else if (std::strcmp(phase, "left") == 0) {
      chromaPhaseX = 0.25f;
      chromaPhaseY = 0.5f;
    } else if (std::strcmp(phase, "top") == 0) {
      chromaPhaseX = 0.5f;
      chromaPhaseY = 0.25f;
    } else if (std::strcmp(phase, "top-left") == 0) {
      chromaPhaseX = chromaPhaseY = 0.25f;
    }
  }

  uint32_t flags = 0u;
  const char *transfer = std::getenv("TFORGE_FSR4_INPUT_TRANSFER");
  // Keep the older boolean as an alias so existing campaign manifests remain
  // reproducible. An explicit supported transfer name takes precedence.
  if ((transfer && std::strcmp(transfer, "rec709") == 0) ||
      (!transfer &&
       std::getenv("TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF"))) {
    flags |= YUV_FLAG_REC709_INPUT_EOTF;
  } else if (transfer && std::strcmp(transfer, "linear") == 0) {
    flags |= YUV_FLAG_LINEAR_INPUT_EOTF;
  }
  if (const char *filter = std::getenv("TFORGE_FSR4_CHROMA_FILTER")) {
    if (std::strcmp(filter, "bilinear") == 0)
      flags |= YUV_FLAG_CHROMA_BILINEAR;
  }
  if (std::getenv("TFORGE_FSR4_PRE_CAS"))
    flags |= YUV_FLAG_PRE_CAS;
  const float reservedFlags = std::bit_cast<float>(flags);

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
          2.0f * (1.0f - kb) * chromaScale,
          chromaPhaseX, chromaPhaseY,
          static_cast<float>(frame.bitDepth), reservedFlags};
}

} // namespace temporal_forge
