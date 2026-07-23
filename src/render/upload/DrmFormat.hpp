// DrmFormat.hpp — DRM/DMABUF format helpers extracted from GpuImageUploader.
//
// Pure free functions (no device/uploader state). Used by the uploader's
// DRM-prime import path to classify frames and pick the matching VkFormat.
#pragma once
#include "media/VideoDecoder.hpp" // for DecodedVideoFrame

#include <cstdint>
#include <vulkan/vulkan.h>

namespace temporal_forge {

// drmFrameMatchesLinearNv12: returns whether a decoded frame is a linear NV12
//                            (or P010/YUV420_10BIT) DRM-prime frame the importer
//                            can fast-path. Also recognizes the split R8+GR88
//                            planar layout some VAAPI drivers emit.
//                            Called by: GpuImageUploader::importAndConvertDrmFrame.
bool drmFrameMatchesLinearNv12(const DecodedVideoFrame &frame);

// drmFourccToVkFormat: map a DRM fourcc (+ 10-bit flag) to the matching Vulkan
//                      2-plane 4:2:0 format. Returns VK_FORMAT_UNDEFINED if
//                      the fourcc isn't a recognized NV12/P010/YUV420 family.
//                      Called by: GpuImageUploader::createImportedDrmRuntime.
VkFormat drmFourccToVkFormat(uint32_t fourcc, bool tenBit);

} // namespace temporal_forge
