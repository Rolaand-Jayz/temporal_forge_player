// DrmFormat.cpp — DRM/DMABUF format helpers for the uploader.
#include "render/upload/DrmFormat.hpp"

#include <libdrm/drm_fourcc.h>

namespace temporal_forge {

bool drmFrameMatchesLinearNv12(const DecodedVideoFrame &frame) {
  if (!frame.hwFrame || frame.drmLayers <= 0 || frame.drmObjects <= 0)
    return false;
  if (frame.drmObjects != 1)
    return false;
  const bool splitNv12 =
      frame.drmLayers >= 2 && frame.drmLayerPlaneCount[0] == 1 &&
      frame.drmLayerPlaneCount[1] == 1 &&
      frame.drmLayerFourcc[0] == DRM_FORMAT_R8 &&
      (frame.drmLayerFourcc[1] == DRM_FORMAT_GR88 ||
       frame.drmLayerFourcc[1] == DRM_FORMAT_RG88);
  if (splitNv12)
    return true;
  if (frame.drmPlanes < 2)
    return false;
  if (frame.drmFourcc == DRM_FORMAT_NV12 ||
      frame.drmFourcc == DRM_FORMAT_YUV420)
    return true;
  if (frame.drmFourcc == DRM_FORMAT_P010 ||
      frame.drmFourcc == DRM_FORMAT_YUV420_10BIT)
    return true;
  return false;
}

VkFormat drmFourccToVkFormat(uint32_t fourcc, bool tenBit) {
  if (fourcc == DRM_FORMAT_NV12 || fourcc == DRM_FORMAT_YUV420) {
    return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  }
  if (fourcc == DRM_FORMAT_P010 || fourcc == DRM_FORMAT_YUV420_10BIT ||
      tenBit) {
    return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

} // namespace temporal_forge
