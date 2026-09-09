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

} // namespace

// ---------------------------------------------------------------------------
// Pure request planning (defect M-05). No Vulkan calls below this point
// until the next anonymous namespace; exercised directly by contract tests.
// ---------------------------------------------------------------------------

VulkanDeviceRequestPlan planVulkanDeviceRequests(
    const std::set<std::string>& availableDeviceExtensions,
    const VulkanFeatureAvailability& availability,
    const VulkanSubgroupSizeBounds& subgroupBounds) {
    VulkanDeviceRequestPlan plan;

    auto has = [&](const char* ext) {
        return availableDeviceExtensions.count(ext) != 0;
    };
    auto missing = [&](const char* ext, const char* why) {
        plan.fatal = true;
        plan.fatalMessage = std::string("Vulkan: required device extension ") +
                            ext + " is not supported by the selected device. " +
                            why +
                            " The player cannot run without it; ensure the "
                            "Vulkan driver (Mesa RADV or AMD proprietary) is "
                            "installed and up to date.";
    };

    // --- BASELINE extensions: the floor for a video player on this platform.
    if (has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        plan.baselineExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    else
        missing(VK_KHR_SWAPCHAIN_EXTENSION_NAME, "Presentation requires it.");
    if (has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
        plan.baselineExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    else
        missing(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                "Zero-copy video frame import (DMABUF) requires it.");
    if (has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
        plan.baselineExtensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    else
        missing(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                "Zero-copy video frame import (DMABUF) requires it.");

    // --- FSR4-CLASS extensions: absence is fine (baseline-only device).
    const bool hasCoopMatrix = has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    if (hasCoopMatrix)
        plan.fsr4ClassExtensions.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    // Fixes the pre-existing internal inconsistency: the subgroup-size-control
    // features were requested while the extension was never enabled.
    const bool hasSubgroupSizeControl = has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    if (hasSubgroupSizeControl)
        plan.fsr4ClassExtensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    plan.fsr4ClassExtensionsPresent = hasCoopMatrix;

    // --- BASELINE features, availability-gated with per-feature tolerance.
    // Tolerable (disable + warn): the compute shaders never use cube arrays,
    // never sample anisotropically (GpuImageUploader sets anisotropyEnable =
    // VK_FALSE), and never decode BC-compressed textures — video frames are
    // uploaded uncompressed.
    plan.enableSamplerAnisotropy = availability.samplerAnisotropy;
    if (!plan.enableSamplerAnisotropy)
        plan.warnings.push_back(
            "samplerAnisotropy unavailable; disabling (no shader uses anisotropic sampling)");
    plan.enableTextureCompressionBC = availability.textureCompressionBC;
    if (!plan.enableTextureCompressionBC)
        plan.warnings.push_back(
            "textureCompressionBC unavailable; disabling (no BC textures are decoded)");
    plan.enableImageCubeArray = availability.imageCubeArray;
    if (!plan.enableImageCubeArray)
        plan.warnings.push_back(
            "imageCubeArray unavailable; disabling (no shader uses cube maps)");

    // Fatal: every FSR/spatial kernel writes storage images, and the model
    // input/output path uses the packed rgb10_a2 extended storage format
    // (easu.comp, postpass_composite.comp bind rgba8/rgb10_a2/rgba16f images).
    plan.enableShaderStorageImageWriteWithoutFormat =
        availability.shaderStorageImageWriteWithoutFormat;
    if (!plan.enableShaderStorageImageWriteWithoutFormat) {
        plan.fatal = true;
        plan.fatalMessage =
            "Vulkan: shaderStorageImageWriteWithoutFormat is not supported by the "
            "selected device. Every compute kernel in the player writes storage "
            "images; the player cannot run without this feature.";
    }
    plan.enableShaderStorageImageExtendedFormats =
        availability.shaderStorageImageExtendedFormats;
    if (!plan.enableShaderStorageImageExtendedFormats) {
        plan.fatal = true;
        plan.fatalMessage =
            "Vulkan: shaderStorageImageExtendedFormats is not supported by the "
            "selected device. The FSR model input/output uses the packed "
            "rgb10_a2 storage format; the player cannot run without this feature.";
    }

    // Tolerable: timelineSemaphore is requested for forward compatibility but
    // no code path currently creates a timeline semaphore (verified: only this
    // request site references it).
    plan.enableTimelineSemaphore = availability.timelineSemaphore;
    if (!plan.enableTimelineSemaphore)
        plan.warnings.push_back(
            "timelineSemaphore unavailable; disabling (no timeline semaphores are created)");

    // Baseline-requested but FSR4-relevant: cooperative-matrix shaders rely on
    // subgroup extended types. Tolerable for the spatial path (disable+warn),
    // but its absence flips the FSR4-class feature summary off.
    plan.enableShaderSubgroupExtendedTypes = availability.shaderSubgroupExtendedTypes;
    if (!plan.enableShaderSubgroupExtendedTypes)
        plan.warnings.push_back(
            "shaderSubgroupExtendedTypes unavailable; disabling (also disables the "
            "FSR4-class feature set)");

    // --- FSR4-CLASS features.
    plan.enableShaderFloat16 = availability.shaderFloat16;
    plan.enableShaderInt8 = availability.shaderInt8;
    plan.enableShaderIntegerDotProduct = availability.shaderIntegerDotProduct;
    plan.enableSubgroupSizeControl =
        hasSubgroupSizeControl && availability.subgroupSizeControl;
    plan.enableComputeFullSubgroups =
        hasSubgroupSizeControl && availability.computeFullSubgroups;
    plan.enableCooperativeMatrix = hasCoopMatrix && availability.cooperativeMatrix;
    plan.fsr4ClassFeaturesPresent =
        plan.enableShaderFloat16 && plan.enableShaderInt8 &&
        plan.enableShaderIntegerDotProduct && plan.enableSubgroupSizeControl &&
        plan.enableComputeFullSubgroups && plan.enableCooperativeMatrix &&
        plan.enableShaderSubgroupExtendedTypes;

    // GLM-NEW-04: only pin requiredSubgroupSize=64 when the control features
    // are enabled and 64 lies within the device's supported bounds.
    plan.requireSubgroupSize64 =
        plan.enableSubgroupSizeControl && plan.enableComputeFullSubgroups &&
        subgroupBounds.known &&
        subgroupBounds.minSize <= 64 && 64 <= subgroupBounds.maxSize;
    if (plan.enableSubgroupSizeControl && !plan.requireSubgroupSize64)
        plan.warnings.push_back(
            "subgroup size control enabled but required size 64 is out of device "
            "bounds; native INT8 pipelines will be built without a required size");

    return plan;
}

bool planAmdRadvPreference(bool driverPropsKnown, bool driverIsRadv,
                           uint32_t vendorId) {
    if (driverPropsKnown) return driverIsRadv; // AMD-but-not-RADV gets no bias
    return vendorId == 0x1002; // ancient-loader fallback (warned at call site)
}

namespace {

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
    // GLM-NEW-07: enumerate instance extensions first. The surface extensions
    // are the platform floor (deliberately unconditional — a missing surface
    // extension makes vkCreateInstance fail with its own actionable error);
    // debug-utils is optional and requested only when actually present.
    std::set<std::string> availInstanceExt;
    {
        uint32_t ec = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &ec, nullptr) ==
            VK_SUCCESS) {
            std::vector<VkExtensionProperties> exts(ec);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &ec, exts.data()) ==
                VK_SUCCESS)
                for (const auto& e : exts) availInstanceExt.insert(e.extensionName);
        }
    }
    std::vector<const char*> instanceExt = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#endif
    };
    if (availInstanceExt.count(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != 0)
        instanceExt.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    else
        logInfo("Vulkan: {} not available; debug messenger disabled",
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
        bool driverPropsKnown = false;
        bool driverIsRadv = false;
        {
            // VkPhysicalDeviceDriverProperties is promotable core since
            // Vulkan 1.2 and the instance is 1.3, so properties2 + the
            // chained driver struct resolves without any extension
            // (GpuCapabilityProbe uses the same working pattern).
            VkPhysicalDeviceProperties2 p2{};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            VkPhysicalDeviceDriverProperties dp{};
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            p2.pNext = &dp;
            vkGetPhysicalDeviceProperties2(pd, &p2);
            driverName = dp.driverName;
            if (dp.driverID != 0) { // 0 == VK_DRIVER_ID_NONE-equivalent (not exposed by this header)
                driverPropsKnown = true;
                driverIsRadv = dp.driverID == VK_DRIVER_ID_MESA_RADV;
            } else {
                logWarn("Vulkan: driver properties unavailable for '{}'; "
                        "falling back to vendor-ID RADV heuristic",
                        d.name);
            }
        }
        // +1000 selection bias applies only to an actual RADV driver;
        // AMD-but-not-RADV (e.g. proprietary) deliberately gets no bias.
        d.amdRadv = planAmdRadvPreference(driverPropsKnown, driverIsRadv, d.vendorId);

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

    // ------------------------------------------------------------------
    // M-05 remediation: enumerate before requesting. Every extension and
    // feature handed to vkCreateDevice below is either verified present or
    // deliberately absent. The decision itself is the pure
    // planVulkanDeviceRequests() above (hermetically contract-tested).
    // ------------------------------------------------------------------
    std::set<std::string> availDevExt;
    {
        uint32_t ec = 0;
        if (vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ec,
                                                 nullptr) != VK_SUCCESS ||
            ec == 0) {
            logError("Vulkan: vkEnumerateDeviceExtensionProperties failed on "
                     "'{}'; cannot verify baseline extensions",
                     info_.name);
            return false;
        }
        std::vector<VkExtensionProperties> exts(ec);
        if (vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ec,
                                                 exts.data()) != VK_SUCCESS) {
            logError("Vulkan: device extension enumeration failed on '{}'",
                     info_.name);
            return false;
        }
        for (const auto& e : exts) availDevExt.insert(e.extensionName);
        logInfo("Vulkan: enumerated {} device extensions on '{}'",
                availDevExt.size(), info_.name);
    }

    // Query what the device supports via one features2 chain.
    VulkanFeatureAvailability avail{};
    {
        VkPhysicalDeviceFeatures2 core{};
        core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceVulkan12Features v12{};
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceSubgroupSizeControlFeatures ssc{};
        ssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
        VkPhysicalDeviceVulkan13Features v13{};
        v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{};
        coop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
        core.pNext = &v12;
        v12.pNext = &ssc;
        ssc.pNext = &v13;
        v13.pNext = &coop;
        vkGetPhysicalDeviceFeatures2(physical_, &core);
        avail.samplerAnisotropy = core.features.samplerAnisotropy == VK_TRUE;
        avail.textureCompressionBC = core.features.textureCompressionBC == VK_TRUE;
        avail.shaderStorageImageWriteWithoutFormat =
            core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;
        avail.imageCubeArray = core.features.imageCubeArray == VK_TRUE;
        avail.shaderStorageImageExtendedFormats =
            core.features.shaderStorageImageExtendedFormats == VK_TRUE;
        avail.timelineSemaphore = v12.timelineSemaphore == VK_TRUE;
        avail.shaderSubgroupExtendedTypes =
            v12.shaderSubgroupExtendedTypes == VK_TRUE;
        avail.shaderFloat16 = v12.shaderFloat16 == VK_TRUE;
        avail.shaderInt8 = v12.shaderInt8 == VK_TRUE;
        avail.shaderIntegerDotProduct = v13.shaderIntegerDotProduct == VK_TRUE;
        avail.subgroupSizeControl = ssc.subgroupSizeControl == VK_TRUE;
        avail.computeFullSubgroups = ssc.computeFullSubgroups == VK_TRUE;
        avail.cooperativeMatrix = coop.cooperativeMatrix == VK_TRUE;
    }
    VulkanSubgroupSizeBounds bounds{};
    if (availDevExt.count(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) != 0) {
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceSubgroupSizeControlProperties sp{};
        sp.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
        p2.pNext = &sp;
        vkGetPhysicalDeviceProperties2(physical_, &p2);
        bounds.known = true;
        bounds.minSize = sp.minSubgroupSize;
        bounds.maxSize = sp.maxSubgroupSize;
    }

    const VulkanDeviceRequestPlan plan =
        planVulkanDeviceRequests(availDevExt, avail, bounds);
    if (plan.fatal) {
        logError("{}", plan.fatalMessage);
        return false;
    }
    for (const auto& w : plan.warnings) logWarn("Vulkan: {}", w);

    std::vector<const char*> devExt = plan.baselineExtensions;
    devExt.insert(devExt.end(), plan.fsr4ClassExtensions.begin(),
                  plan.fsr4ClassExtensions.end());

    VkPhysicalDeviceFeatures2 feats2{};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.features.samplerAnisotropy =
        plan.enableSamplerAnisotropy ? VK_TRUE : VK_FALSE;
    feats2.features.textureCompressionBC =
        plan.enableTextureCompressionBC ? VK_TRUE : VK_FALSE;
    feats2.features.shaderStorageImageWriteWithoutFormat =
        plan.enableShaderStorageImageWriteWithoutFormat ? VK_TRUE : VK_FALSE;
    // The FSR model input is VK_FORMAT_A2B10G10R10_UNORM_PACK32. Vulkan
    // classifies that packed format as an extended storage-image format, so
    // every shader that writes/reads the RGB10 image depends on this feature.
    // Without it, RGBA8 paths still work while RGB10 writes can silently
    // remain zero on RADV. Availability-gated; absence is fatal because the
    // model path has no RGBA8 substitute for its packed I/O.
    feats2.features.shaderStorageImageExtendedFormats =
        plan.enableShaderStorageImageExtendedFormats ? VK_TRUE : VK_FALSE;
    feats2.features.imageCubeArray =
        plan.enableImageCubeArray ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.shaderFloat16 = plan.enableShaderFloat16 ? VK_TRUE : VK_FALSE;
    v12.shaderInt8 = plan.enableShaderInt8 ? VK_TRUE : VK_FALSE;
    v12.timelineSemaphore = plan.enableTimelineSemaphore ? VK_TRUE : VK_FALSE;
    v12.shaderSubgroupExtendedTypes =
        plan.enableShaderSubgroupExtendedTypes ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    subgroup.subgroupSizeControl = plan.enableSubgroupSizeControl ? VK_TRUE : VK_FALSE;
    subgroup.computeFullSubgroups = plan.enableComputeFullSubgroups ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.shaderIntegerDotProduct =
        plan.enableShaderIntegerDotProduct ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative{};
    cooperative.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    cooperative.cooperativeMatrix = plan.enableCooperativeMatrix ? VK_TRUE : VK_FALSE;

    // Chain only structs whose extension is enabled: chaining a feature
    // struct for a non-enabled extension is invalid usage.
    feats2.pNext = &v12;
    if (availDevExt.count(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) != 0) {
        v12.pNext = &subgroup;
        subgroup.pNext = &v13;
    } else {
        v12.pNext = &v13;
    }
    if (availDevExt.count(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) != 0)
        v13.pNext = &cooperative;

    // Capability summary for downstream backend gating.
    caps_ = VulkanCaps{};
    caps_.fsr4ClassExtensions = plan.fsr4ClassExtensionsPresent;
    caps_.fsr4ClassFeatures = plan.fsr4ClassFeaturesPresent;
    caps_.requireSubgroupSize64 = plan.requireSubgroupSize64;
    caps_.subgroupBounds = bounds;
    caps_.amdVendor = info_.vendorId == 0x1002;
    caps_.amdRadvDriver = info_.amdRadv;
    caps_.driverPropsKnown = true;

    logInfo("Vulkan: enabled device extensions ({}):", devExt.size());
    for (const char* e : devExt) logInfo("Vulkan:   ext {}", e);
    logInfo("Vulkan: fsr4ClassExtensions={} fsr4ClassFeatures={} "
            "requireSubgroupSize64={} (bounds {}..{})",
            caps_.fsr4ClassExtensions, caps_.fsr4ClassFeatures,
            caps_.requireSubgroupSize64, bounds.minSize, bounds.maxSize);

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
