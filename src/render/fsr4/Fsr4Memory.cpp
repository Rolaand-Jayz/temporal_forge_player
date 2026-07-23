// Fsr4Memory.cpp — Vulkan memory-type helper for the FSR4 harness.
#include "render/fsr4/Fsr4Memory.hpp"

namespace temporal_forge {

uint32_t findMemoryTypeFor(VkPhysicalDevice physical, uint32_t typeBits,
                           VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(physical, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) &&
        (m.memoryTypes[i].propertyFlags & props) == props)
      return i;
  }
  return ~0u;
}

} // namespace temporal_forge
