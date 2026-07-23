// GpuCapabilityProbe.cpp
#include "backend/GpuCapabilityProbe.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace temporal_forge {

const char* GpuCapabilityProbe::profileName(Fsr4Profile p) {
    switch (p) {
        case Fsr4Profile::Unsupported: return "Unsupported";
        case Fsr4Profile::Int8Dot4:    return "INT8 DOT4 (RDNA3 cooperative matrix, SINT8->SINT32)";
        case Fsr4Profile::Fp8Wmma:     return "FP8 WMMA (RDNA4 native)";
        case Fsr4Profile::Fp16Fallback:return "FP16 fallback (FLOAT16->FLOAT32)";
    }
    return "?";
}

namespace {

// Heuristic RDNA-family detection from device name + driver.
struct GpuFamily { bool rdna3 = false; bool rdna4 = false; bool amd = false; bool radv = false; };

GpuFamily detectFamily(const std::string& deviceName, const std::string& driverName) {
    GpuFamily f;
    auto has = [&](const char* needle) {
        return std::search(deviceName.begin(), deviceName.end(),
                           needle, needle + std::strlen(needle),
                           [](char a, char b){ return std::tolower(a) == std::tolower(b); })
               != deviceName.end();
    };
    f.amd = std::strstr(deviceName.c_str(), "AMD") ||
            std::strstr(deviceName.c_str(), "Radeon") ||
            std::strstr(deviceName.c_str(), "NAVI") ||
            std::strstr(deviceName.c_str(), "Navi");
    f.radv = std::strstr(driverName.c_str(), "radv") ||
             std::strstr(driverName.c_str(), "RADV");
    f.rdna3 = has("7900") || has("7800") || has("7700") || has("7600") ||
              has("7500") || has("RX 7") || has("NAVI3") || has("Navi 3") || has("navi3");
    f.rdna4 = has("9070") || has("9050") || has("RX 9") || has("NAVI4") ||
              has("Navi 4") || has("navi4");
    return f;
}

VkPhysicalDevice pickBestAmd(VkInstance instance) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(instance, &n, nullptr) != VK_SUCCESS || n == 0)
        return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    VkPhysicalDevice best = VK_NULL_HANDLE; int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties2 p2{}; p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceDriverProperties dp{}; dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        p2.pNext = &dp;
        vkGetPhysicalDeviceProperties2(d, &p2);
        auto fam = detectFamily(p2.properties.deviceName, dp.driverName);
        int score = 0;
        if (fam.amd && fam.radv) score += 1000;
        if (fam.rdna4) score += 600; else if (fam.rdna3) score += 500;
        if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 200;
        if (score > bestScore) { bestScore = score; best = d; }
    }
    return best;
}

} // namespace

bool GpuCapabilityProbe::detectInt8CooperativeMatrix(VkPhysicalDevice device,
                                                      VkInstance instanceForLookup,
                                                      GpuCapability& out) {
    uint32_t ec = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &ec, nullptr);
    std::vector<VkExtensionProperties> exts(ec);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &ec, exts.data());
    bool hasCM = false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0) {
            hasCM = true; break;
        }
    }
    out.hasCooperativeMatrix = hasCM;
    if (!hasCM) { out.failReason = "VK_KHR_cooperative_matrix not exposed"; return false; }

    // Feature check.
    VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmf{};
    cmf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    f2.pNext = &cmf;
    vkGetPhysicalDeviceFeatures2(device, &f2);
    if (cmf.cooperativeMatrix == VK_FALSE) {
        out.failReason = "cooperativeMatrix feature not supported";
        return false;
    }

    // Resolve the properties query function via the instance (cm_dump proved
    // this resolves correctly when given a real instance).
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR getProps = nullptr;
    if (instanceForLookup != VK_NULL_HANDLE) {
        getProps = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
            vkGetInstanceProcAddr(instanceForLookup,
                "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    }
    if (!getProps) {
        out.failReason = "cannot resolve vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR";
        return false;
    }

    // Enumerate every mode the device exposes; pick the best fit for the RE
    // quantization scheme. RE §4.2: integer MAC on uint8 codebook indices
    // produces valid float32 bit patterns. The natural cooperative-matrix
    // The shaders use symmetric signed quantization, so select the matching
    // SINT8xSINT8 -> SINT32 mode. FP16xFP16->F32 is recorded as a fallback.
    uint32_t mc = 0;
    if (getProps(device, &mc, nullptr) != VK_SUCCESS || mc == 0) {
        out.failReason = "no cooperative matrix modes reported";
        return false;
    }
    std::vector<VkCooperativeMatrixPropertiesKHR> props(mc);
    for (auto& pr : props) {
        pr.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        pr.pNext = nullptr;
    }
    if (getProps(device, &mc, props.data()) != VK_SUCCESS) {
        out.failReason = "cooperative matrix properties query failed";
        return false;
    }

    for (const auto& pr : props) {
        // Match the signed cooperative-matrix types instantiated by GLSL.
        if (!out.hasUint8Input &&
            pr.AType == VK_COMPONENT_TYPE_SINT8_KHR &&
            pr.BType == VK_COMPONENT_TYPE_SINT8_KHR &&
            pr.CType == VK_COMPONENT_TYPE_SINT32_KHR &&
            pr.ResultType == VK_COMPONENT_TYPE_SINT32_KHR) {
            out.hasUint8Input = true;
            out.hasInt32Accum = true;
            out.int8Mode = {pr.AType, pr.BType, pr.CType, pr.ResultType,
                            {pr.MSize, pr.NSize}, pr.saturatingAccumulation != VK_FALSE};
            out.cooperativeMatrixTileSize = {pr.MSize, pr.NSize};
        }
        // FP16 fallback: FLOAT16 x FLOAT16 -> FLOAT32.
        if (!out.hasFp16Fallback &&
            pr.AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            pr.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            pr.CType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
            pr.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR) {
            out.hasFp16Fallback = true;
            out.fp16Mode = {pr.AType, pr.BType, pr.CType, pr.ResultType,
                            {pr.MSize, pr.NSize}, pr.saturatingAccumulation != VK_FALSE};
        }
    }

    if (!out.hasUint8Input) {
        out.failReason = "no SINT8xSINT8->SINT32 cooperative matrix mode "
                         "(RDNA3 INT8 DOT4 path requires it for the RE codebook)";
        return false;
    }
    return true;
}

GpuCapability GpuCapabilityProbe::probe(VkInstance instance) {
    VkPhysicalDevice dev = pickBestAmd(instance);
    if (dev == VK_NULL_HANDLE) {
        GpuCapability g;
        g.failReason = "no AMD physical device found";
        return g;
    }
    return probe(dev, instance);
}

GpuCapability GpuCapabilityProbe::probe(VkPhysicalDevice device, VkInstance instanceForLookup) {
    GpuCapability g;
    g.physicalDevice = device;
    if (device == VK_NULL_HANDLE) { g.failReason = "null physical device"; return g; }

    VkPhysicalDeviceProperties2 p2{}; p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    VkPhysicalDeviceDriverProperties dp{}; dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    p2.pNext = &dp;
    vkGetPhysicalDeviceProperties2(device, &p2);
    g.deviceName = p2.properties.deviceName;
    g.apiVersion = p2.properties.apiVersion;
    g.driverName = dp.driverName;
    g.driverInfo = dp.driverInfo;

    auto fam = detectFamily(g.deviceName, g.driverName);
    g.isRdna3 = fam.rdna3;
    g.isRdna4 = fam.rdna4;

    if (!fam.amd) {
        g.failReason = "not an AMD GPU (FSR 4.1 INT8 targets RDNA3/4)";
        logWarn("GpuCapabilityProbe: {} — {}", g.deviceName, g.failReason);
        return g;
    }

    if (!detectInt8CooperativeMatrix(device, instanceForLookup, g)) {
        logWarn("GpuCapabilityProbe: {} — INT8 cooperative matrix unavailable: {}",
                g.deviceName, g.failReason);
        return g;
    }

    // Resolve profile: RDNA3 -> Int8Dot4 (SINT8->SINT32). FP16 available as
    // a secondary path if we later derive an FP16 weight encoding.
    g.profile = Fsr4Profile::Int8Dot4;
    g.valid = true;
    logInfo("GpuCapabilityProbe: {} ({}) -> {} [tile {}x{}, SINT8->SINT32 accum]{}",
            g.deviceName, g.driverName, profileName(g.profile),
            g.cooperativeMatrixTileSize.width, g.cooperativeMatrixTileSize.height,
            g.hasFp16Fallback ? " + FP16 fallback available" : "");
    return g;
}

} // namespace temporal_forge
