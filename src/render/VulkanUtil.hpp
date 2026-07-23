// VulkanUtil.hpp — shared Vulkan allocation helpers used by the render layer.
//
// findMemoryType(): resolves a memory type index satisfying type-bits + property
//                   flags. Used by every buffer/image allocator.
// createImage():    creates a VkImage + VkImageView + binds+dedicates memory.
// createBuffer():   creates a VkBuffer + binds+dedicates memory.
//
// All allocators use dedicated-allocation (one VkDeviceMemory per resource).
// This is simple and correct; sub-allocation is not warranted at the FSR4
// resource count (~10 images + a handful of buffers per source size).
#pragma once
#include "util/Log.hpp"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace temporal_forge {

// Returns ~0u if no memory type satisfies both constraints.
inline uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeBits,
                               VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(physical, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (m.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return ~0u;
}

struct GpuImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    uint32_t       mipCount = 1;
    VkImageLayout   layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Create a 2D image with a dedicated allocation + a basic color attachment /
// storage / sample view. Returns true on success. Fills `out`.
// `usage` must include at least one of SAMPLED/STORAGE/COLOR_ATTACHMENT/TRANSFER_DST.
// `initialLayout` is UNDEFINED (caller must transition with a pipeline barrier).
inline bool createGpuImage(VkDevice device, VkPhysicalDevice physical,
                           uint32_t width, uint32_t height, VkFormat format,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect,
                           GpuImage& out, const char* tag,
                           uint32_t queueFamilyA = VK_QUEUE_FAMILY_IGNORED,
                           uint32_t queueFamilyB = VK_QUEUE_FAMILY_IGNORED) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    const uint32_t queueFamilies[2] = {queueFamilyA, queueFamilyB};
    const bool concurrent = queueFamilyA != VK_QUEUE_FAMILY_IGNORED &&
                            queueFamilyB != VK_QUEUE_FAMILY_IGNORED &&
                            queueFamilyA != queueFamilyB;
    ici.sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT
                                 : VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = concurrent ? 2u : 0u;
    ici.pQueueFamilyIndices = concurrent ? queueFamilies : nullptr;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &out.image) != VK_SUCCESS) {
        logError("VulkanUtil: createImage '{}' failed", tag);
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, out.image, &mr);
    uint32_t memType = findMemoryType(physical, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == ~0u) {
        logError("VulkanUtil: no device-local memory type for '{}'", tag);
        vkDestroyImage(device, out.image, nullptr); out.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        logError("VulkanUtil: allocateImageMemory '{}' failed", tag);
        vkDestroyImage(device, out.image, nullptr); out.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(device, out.image, out.memory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &vci, nullptr, &out.view) != VK_SUCCESS) {
        logError("VulkanUtil: createImageView '{}' failed", tag);
        vkFreeMemory(device, out.memory, nullptr); out.memory = VK_NULL_HANDLE;
        vkDestroyImage(device, out.image, nullptr); out.image = VK_NULL_HANDLE;
        return false;
    }
    out.format = format;
    out.width = width;
    out.height = height;
    out.mipCount = 1;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

inline void destroyGpuImage(VkDevice device, GpuImage& img) {
    if (device == VK_NULL_HANDLE) return;
    if (img.view   != VK_NULL_HANDLE) { vkDestroyImageView(device, img.view, nullptr);   img.view   = VK_NULL_HANDLE; }
    if (img.image  != VK_NULL_HANDLE) { vkDestroyImage(device, img.image, nullptr);      img.image  = VK_NULL_HANDLE; }
    if (img.memory != VK_NULL_HANDLE) { vkFreeMemory(device, img.memory, nullptr);       img.memory = VK_NULL_HANDLE; }
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

struct GpuBuffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
};

inline bool createGpuBufferObj(VkDevice device, VkPhysicalDevice physical,
                               VkDeviceSize size, VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags memProps,
                               GpuBuffer& out, const char* tag) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        logError("VulkanUtil: createBuffer '{}' failed", tag);
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device, out.buffer, &mr);
    uint32_t memType = findMemoryType(physical, mr.memoryTypeBits, memProps);
    if (memType == ~0u) {
        logError("VulkanUtil: no memory type for '{}'", tag);
        vkDestroyBuffer(device, out.buffer, nullptr); out.buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        logError("VulkanUtil: allocateBufferMemory '{}' failed", tag);
        vkDestroyBuffer(device, out.buffer, nullptr); out.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    out.size = mr.size;
    return true;
}

inline void destroyGpuBufferObj(VkDevice device, GpuBuffer& buf) {
    if (device == VK_NULL_HANDLE) return;
    if (buf.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buf.buffer, nullptr); buf.buffer = VK_NULL_HANDLE; }
    if (buf.memory != VK_NULL_HANDLE) { vkFreeMemory(device, buf.memory, nullptr);   buf.memory = VK_NULL_HANDLE; }
    buf.size = 0;
}

// One-shot submit+wait helper. Records nothing — caller records into cmd then
// calls this. Useful for the synchronous upload/readback paths.
inline bool submitAndWait(VkDevice device, VkQueue queue, VkCommandBuffer cmd,
                          VkFence fence) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);
    return true;
}

} // namespace temporal_forge
