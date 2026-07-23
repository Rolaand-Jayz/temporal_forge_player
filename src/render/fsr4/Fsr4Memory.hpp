// Fsr4Memory.hpp — small Vulkan memory-type helper used by the FSR4 harness.
//
// Extracted from Fsr4DispatchHarness.cpp. Pure free function (no class state).
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace temporal_forge {

// findMemoryTypeFor: pick a VkMemoryType matching typeBits and the requested
//                    property flags. Called by Fsr4DispatchHarness buffer/image
//                    allocation helpers. Returns ~0u if none matches.
uint32_t findMemoryTypeFor(VkPhysicalDevice physical, uint32_t typeBits,
                           VkMemoryPropertyFlags props);

} // namespace temporal_forge
