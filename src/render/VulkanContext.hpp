// VulkanContext.hpp — owns the VkInstance / VkPhysicalDevice / VkDevice /
// graphics+compute queue used by the entire render + FSR pipeline.
//
// spec 01: "Primary GPU: AMD RDNA2/3/4, RADV first."
// spec 00: renderer is Vulkan. Phase 0 acceptance: a Vulkan device is
// selected and logged.
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace temporal_forge {

struct GpuDeviceInfo {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    // RADV/AMD preference: true if this looks like a discrete AMD GPU on RADV.
    bool amdRadv = false;
    size_t dedicatedVramBytes = 0;
    uint32_t graphicsFamily = ~0u;
    uint32_t computeFamily = ~0u;
    uint32_t transferFamily = ~0u;
};

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // init: create the instance (or adopt sharedInstance from Qt), pick the
    //       best physical device (AMD/RADV preferred), and create a logical
    //       device with a graphics+compute queue.
    //
    // Called by: main.cpp at startup. When sharedInstance is Qt's QVulkanInstance,
    //            the FSR4 compute device shares instance identity with presentation.
    // Calls:     pickPhysicalDevice, createLogicalDevice. enableValidation adds the
    //            validation layer when TFORGE_VK_VALIDATE is set.
    // Returns:   false on failure (valid() then stays false; the engine degrades
    //            to raw frames with no upscaling).
    bool init(bool enableValidation, VkInstance sharedInstance = VK_NULL_HANDLE);

    // shutdown: destroy the logical device + command pools + instance (if owned).
    //          Called by: dtor (and explicitly if init fails partway).
    void shutdown();

    // Trivial accessors for the selected device + queues. valid() reports whether
    // init succeeded. queueFamily/computeQueueFamily expose the queue families
    // used by GpuImageUploader + Fsr4DispatchHarness.
    [[nodiscard]] bool valid() const { return device_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] uint32_t queueFamily() const { return queueFamily_; }
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] uint32_t computeQueueFamily() const { return computeQueueFamily_; }
    [[nodiscard]] VkQueue computeQueue() const { return computeQueue_; }
    [[nodiscard]] const GpuDeviceInfo& info() const { return info_; }

    // commandPool / transientPool: lazy single command pools for long-lived vs.
    //   transient (reset-per-frame) command buffers. Called by GpuImageUploader
    //   and Fsr4DispatchHarness. transientPool is RESET_COMMAND_BUFFER flagged.
    VkCommandPool commandPool();
    VkCommandPool transientPool();

private:
    // pickPhysicalDevice: enumerate and rank physical devices (AMD/RADV first,
    //                     then discrete, then any with graphics+compute).
    //                     Called by: init.
    bool pickPhysicalDevice();
    // createLogicalDevice: create the VkDevice + queues for the picked physical device.
    //                      Called by: init.
    bool createLogicalDevice();
    // destroyPools: tear down the command pools. Called by: shutdown.
    void destroyPools();

    VkInstance instance_ = VK_NULL_HANDLE;
    bool ownsInstance_ = true;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = ~0u;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t computeQueueFamily_ = ~0u;
    VkQueue computeQueue_ = VK_NULL_HANDLE;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandPool transientPool_ = VK_NULL_HANDLE;

    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    GpuDeviceInfo info_;
};

} // namespace temporal_forge
