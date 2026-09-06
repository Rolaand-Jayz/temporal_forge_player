// VulkanContext.cpp
#include "render/VulkanContext.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace temporal_forge {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        logError("VUID: {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        logWarn("VUID: {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        logDebug("VUID: {}", data->pMessage);
    return VK_FALSE;
}

bool isAmdRadv(std::string_view name, uint32_t vendorId, std::string_view driverName) {
    // AMD vendor id 0x1002. RADV driver name surfaces in driver properties.
    if (vendorId == 0x1002) {
        // We can't query VkPhysicalDeviceDriverProperties before creating a
        // device; rely on device name heuristics in addition to vendor id.
        return true;
    }
    const std::string lower(name);
    (void)lower;
    return driverName.find("radv") != std::string_view::npos ||
           driverName.find("RADV") != std::string_view::npos;
}

uint32_t findQueueFamily(VkPhysicalDevice pd, VkQueueFlags required) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());

    // Prefer a family that has the required flags AND graphics (so we can
    // present from the same queue). Fallback: any family with required flags.
    uint32_t fallback = ~0u;
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & required) == required) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
            if (fallback == ~0u) fallback = i;
        }
    }
    return fallback;
}

uint32_t findDedicatedComputeFamily(VkPhysicalDevice pd) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    uint32_t fallback = ~0u;
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) return i;
        if (fallback == ~0u) fallback = i;
    }
    return fallback;
}

int scoreDevice(const GpuDeviceInfo& d) {
    // spec: AMD RADV discrete first. Score reflects that preference.
    int score = 0;
    if (d.amdRadv) score += 1000;
    if (d.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 500;
    else if (d.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
    score += static_cast<int>(std::min<size_t>(d.dedicatedVramBytes / (1024ull * 1024ull), 200));
    return score;
}

} // namespace

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() { shutdown(); }

bool VulkanContext::init(bool enableValidation, VkInstance sharedInstance) {
    if (sharedInstance != VK_NULL_HANDLE) {
        instance_ = sharedInstance;
        ownsInstance_ = false;
        logInfo("Vulkan: using Qt shared instance");
        if (!pickPhysicalDevice()) return false;
        if (!createLogicalDevice()) return false;
        logInfo("Vulkan: device '{}' selected, queue family {}", info_.name, queueFamily_);
        return true;
    }
    // --- instance ---
    std::vector<const char*> layers;
    std::vector<const char*> instanceExt = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#endif
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    if (enableValidation) {
        uint32_t lc = 0;
        vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> avail(lc);
        vkEnumerateInstanceLayerProperties(&lc, avail.data());
        for (const auto& a : avail) {
            if (std::strcmp(a.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Temporal Forge Player";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Temporal Forge";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!layers.empty()) { ici.enabledLayerCount = layers.size(); ici.ppEnabledLayerNames = layers.data(); }
    ici.enabledExtensionCount = instanceExt.size();
    ici.ppEnabledExtensionNames = instanceExt.data();

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        logError("Vulkan: vkCreateInstance failed");
        return false;
    }
    logInfo("Vulkan: instance created (api 1.3)");

    if (enableValidation && !layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (fn) fn(instance_, &dci, nullptr, &debugMessenger_);
    }

    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;

    logInfo("Vulkan: device '{}' selected, queue family {}", info_.name, queueFamily_);
    return true;
}

bool VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { logError("Vulkan: no physical devices"); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    std::vector<GpuDeviceInfo> candidates;
    for (VkPhysicalDevice pd : devices) {
        GpuDeviceInfo d{};
        d.physical = pd;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        d.name = props.deviceName;
        d.vendorId = props.vendorID;
        d.deviceId = props.deviceID;
        d.type = props.deviceType;

        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mem);
        size_t vram = 0;
        for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                vram += mem.memoryHeaps[h].size;
        d.dedicatedVramBytes = vram;

        d.graphicsFamily = findQueueFamily(pd, VK_QUEUE_GRAPHICS_BIT);
        d.computeFamily = findDedicatedComputeFamily(pd);
        d.transferFamily = findQueueFamily(pd, VK_QUEUE_TRANSFER_BIT);

        std::string driverName;
#if defined(VK_KHR_driver_properties)
        // Driver properties need the extension struct chained; probe via
        // property2 if available, otherwise fall back to vendor/name heuristics.
#endif
        d.amdRadv = isAmdRadv(d.name, d.vendorId, driverName);

        candidates.push_back(d);
        logDebug("Vulkan: candidate '{}' vendor={:#x} type={} vram={}MiB{}",
                 d.name, d.vendorId, static_cast<int>(d.type),
                 d.dedicatedVramBytes / (1024*1024),
                 d.amdRadv ? " [AMD/RADV]" : "");
    }

    if (candidates.empty()) { logError("Vulkan: no usable devices"); return false; }
    std::sort(candidates.begin(), candidates.end(),
              [](const GpuDeviceInfo& a, const GpuDeviceInfo& b) {
                  return scoreDevice(a) > scoreDevice(b);
              });
    info_ = candidates.front();
    physical_ = info_.physical;
    return true;
}

bool VulkanContext::createLogicalDevice() {
    if (info_.graphicsFamily == ~0u) {
        logError("Vulkan: no graphics queue family on selected device");
        return false;
    }
    queueFamily_ = info_.graphicsFamily;
    computeQueueFamily_ = info_.computeFamily == ~0u ? queueFamily_ : info_.computeFamily;

    float prio = 1.0f;
    std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
    queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfos[0].queueFamilyIndex = queueFamily_;
    queueInfos[0].queueCount = 1;
    queueInfos[0].pQueuePriorities = &prio;
    uint32_t queueInfoCount = 1;
    if (computeQueueFamily_ != queueFamily_) {
        queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[1].queueFamilyIndex = computeQueueFamily_;
        queueInfos[1].queueCount = 1;
        queueInfos[1].pQueuePriorities = &prio;
        queueInfoCount = 2;
    }

    std::vector<const char*> devExt = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    // Optional: timeline semaphores, dynamic rendering (core in 1.2+, but keep
    // the extension name out since we request api 1.3).

    VkPhysicalDeviceFeatures2 feats2{};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(physical_, &supportedFeatures);
    feats2.features.samplerAnisotropy = VK_TRUE;
    feats2.features.textureCompressionBC = VK_TRUE;
    feats2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // The FSR model input is VK_FORMAT_A2B10G10R10_UNORM_PACK32. Vulkan
    // classifies that packed format as an extended storage-image format, so
    // every shader that writes/reads the RGB10 image depends on this feature.
    // Without it, RGBA8 paths still work while RGB10 writes can silently
    // remain zero on RADV. Enable it only when the selected device advertises
    // support; callers can then fall back if the packed model path is absent.
    feats2.features.shaderStorageImageExtendedFormats =
        supportedFeatures.shaderStorageImageExtendedFormats;
    feats2.features.imageCubeArray = VK_TRUE;
    logInfo("Vulkan: shaderStorageImageExtendedFormats={}",
            feats2.features.shaderStorageImageExtendedFormats == VK_TRUE);

    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.shaderFloat16 = VK_TRUE;
    v12.shaderInt8 = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    v12.shaderSubgroupExtendedTypes = VK_TRUE;
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    subgroup.subgroupSizeControl = VK_TRUE;
    subgroup.computeFullSubgroups = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.shaderIntegerDotProduct = VK_TRUE;
    feats2.pNext = &v12;

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative{};
    cooperative.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    cooperative.cooperativeMatrix = VK_TRUE;
    v12.pNext = &subgroup;
    subgroup.pNext = &v13;
    v13.pNext = &cooperative;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &feats2;
    dci.queueCreateInfoCount = queueInfoCount;
    dci.pQueueCreateInfos = queueInfos.data();
    dci.enabledExtensionCount = devExt.size();
    dci.ppEnabledExtensionNames = devExt.data();

    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        logError("Vulkan: vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    vkGetDeviceQueue(device_, computeQueueFamily_, 0, &computeQueue_);
    logInfo("Vulkan: graphics queue family {}, FSR compute queue family {}",
            queueFamily_, computeQueueFamily_);
    return true;
}

VkCommandPool VulkanContext::commandPool() {
    if (cmdPool_ == VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = queueFamily_;
        vkCreateCommandPool(device_, &ci, nullptr, &cmdPool_);
    }
    return cmdPool_;
}

VkCommandPool VulkanContext::transientPool() {
    if (transientPool_ == VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        ci.queueFamilyIndex = queueFamily_;
        vkCreateCommandPool(device_, &ci, nullptr, &transientPool_);
    }
    return transientPool_;
}

void VulkanContext::destroyPools() {
    if (device_ != VK_NULL_HANDLE) {
        if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
        if (transientPool_) vkDestroyCommandPool(device_, transientPool_, nullptr);
        cmdPool_ = transientPool_ = VK_NULL_HANDLE;
    }
}

void VulkanContext::shutdown() {
    destroyPools();
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        computeQueue_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && ownsInstance_) {
        vkDestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
    ownsInstance_ = true;
}

} // namespace temporal_forge
