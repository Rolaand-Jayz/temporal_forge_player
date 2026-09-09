// VulkanContext.hpp — owns the VkInstance / VkPhysicalDevice / VkDevice /
// graphics+compute queue used by the entire render + FSR pipeline.
//
// spec 01: "Primary GPU: AMD RDNA2/3/4, RADV first."
// spec 00: renderer is Vulkan. Phase 0 acceptance: a Vulkan device is
// selected and logged.
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace temporal_forge {

// ---------------------------------------------------------------------------
// Pure device-request planning (defect M-05 remediation).
//
// These types and functions make NO Vulkan calls. Given the enumerated
// device-extension names, the queried feature availability, and the device's
// required-subgroup-size bounds, they produce the exact request set that
// createLogicalDevice will pass to vkCreateDevice. Keeping the decision pure
// makes the baseline-vs-FSR4-class split hermetically testable.
// ---------------------------------------------------------------------------

// What the selected physical device actually supports, flattened from
// vkGetPhysicalDeviceFeatures2 (core + v12 + subgroup-size-control + v13 +
// cooperative-matrix chains).
struct VulkanFeatureAvailability {
    // Baseline class (spatial player path).
    bool samplerAnisotropy = false;
    bool textureCompressionBC = false;
    bool shaderStorageImageWriteWithoutFormat = false;
    bool imageCubeArray = false;
    bool shaderStorageImageExtendedFormats = false;
    bool timelineSemaphore = false;
    bool shaderSubgroupExtendedTypes = false;
    // FSR4-class.
    bool shaderFloat16 = false;
    bool shaderInt8 = false;
    bool shaderIntegerDotProduct = false;
    bool subgroupSizeControl = false;
    bool computeFullSubgroups = false;
    bool cooperativeMatrix = false;
};

// VkPhysicalDeviceSubgroupSizeControlProperties flattened. `known` is false
// when VK_EXT_subgroup_size_control is not exposed (properties chain absent).
struct VulkanSubgroupSizeBounds {
    bool known = false;
    uint32_t minSize = 0;
    uint32_t maxSize = 0;
};

// The adjudicated request set. `fatal` means init must fail with the
// actionable `fatalMessage` (a missing baseline necessity). FSR4-class
// absence never sets fatal — the device is created baseline-only and the
// fsr4-class flags stay false.
struct VulkanDeviceRequestPlan {
    bool fatal = false;
    std::string fatalMessage;
    std::vector<const char*> baselineExtensions;
    std::vector<const char*> fsr4ClassExtensions;
    // Baseline feature enables (all availability-gated).
    bool enableSamplerAnisotropy = false;
    bool enableTextureCompressionBC = false;
    bool enableShaderStorageImageWriteWithoutFormat = false;
    bool enableImageCubeArray = false;
    bool enableShaderStorageImageExtendedFormats = false;
    bool enableTimelineSemaphore = false;
    bool enableShaderSubgroupExtendedTypes = false;
    // FSR4-class feature enables (require the corresponding availability).
    bool enableShaderFloat16 = false;
    bool enableShaderInt8 = false;
    bool enableShaderIntegerDotProduct = false;
    bool enableSubgroupSizeControl = false;
    bool enableComputeFullSubgroups = false;
    bool enableCooperativeMatrix = false;
    // Class summaries consumed by backend selection.
    bool fsr4ClassExtensionsPresent = false; // VK_KHR_cooperative_matrix
    bool fsr4ClassFeaturesPresent = false;   // every FSR4-class feature
    // GLM-NEW-04: request requiredSubgroupSize=64 in the native-INT8
    // pipelines only when the features were enabled and 64 is within bounds.
    bool requireSubgroupSize64 = false;
    std::vector<std::string> warnings;
};

// Decide the full request set from enumerated availability. Pure.
[[nodiscard]] VulkanDeviceRequestPlan planVulkanDeviceRequests(
    const std::set<std::string>& availableDeviceExtensions,
    const VulkanFeatureAvailability& availability,
    const VulkanSubgroupSizeBounds& subgroupBounds);

// GLM defect (device-selection bias): the +1000 RADV preference applies only
// to an actual RADV driver. driverPropsKnown=false means the driver-properties
// query was unavailable (ancient loader) and the vendor-ID heuristic is used.
[[nodiscard]] bool planAmdRadvPreference(bool driverPropsKnown,
                                         bool driverIsRadv,
                                         uint32_t vendorId);

// Capability summary exposed after init. FSR4-class flags are false on a
// baseline-only device; consumers must treat that as "FSR4-RE unavailable",
// never as an init failure.
struct VulkanCaps {
    bool fsr4ClassExtensions = false;
    bool fsr4ClassFeatures = false;
    bool requireSubgroupSize64 = false;
    VulkanSubgroupSizeBounds subgroupBounds;
    bool amdVendor = false;     // vendorId == 0x1002
    bool amdRadvDriver = false; // driverId == VK_DRIVER_ID_MESA_RADV
    bool driverPropsKnown = false;
};

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
    // Capability summary of the created logical device (M-05 remediation).
    // Valid only after init() returns true.
    [[nodiscard]] const VulkanCaps& caps() const { return caps_; }

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
    VulkanCaps caps_;
};

} // namespace temporal_forge
