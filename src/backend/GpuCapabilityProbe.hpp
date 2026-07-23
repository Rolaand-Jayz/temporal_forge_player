// GpuCapabilityProbe.hpp — detects the FSR 4.1 INT8 RDNA3 execution profile.
//
// FSR 4.1.1 is officially supported on RDNA3 (AMD Redstone SDK 2.3, July 2026).
// RDNA3 has no native FP8 WMMA, so the path runs as INT8 cooperative-matrix
// DOT4 with FP32 accumulation — the mechanism AMD and the community document
// (CachyOS forum: "a path relying on INT8 and float16 cooperative matrix
// support").
//
// This probe queries Vulkan for:
//   - VK_KHR_cooperative_matrix availability
//   - VkCooperativeMatrixPropertiesKHR with VK_COMPONENT_TYPE_SINT8_KHR
//     input and VK_COMPONENT_TYPE_FLOAT32_KHR accumulator
//   - the RDNA3 INT8 DOT4 profile (f8_mask_int8_dot4)
//
// On RX 7000 / RDNA3 / RADV the probe resolves to Profile::Int8Dot4. On RDNA4
// (if present) it would resolve to Fp8Wmma (future). If nothing matches, the
// Fsr4Int8Backend reports UnsupportedDevice and the selector falls through to
// FSR 3.1.5.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace temporal_forge {

enum class Fsr4Profile : uint8_t {
    Unsupported,    // no viable matrix path — backend will decline
    Int8Dot4,       // RDNA3: 8-bit cooperative-matrix DOT4, integer accumulate,
                    //   bitcast to float32 post-accumulation (RE §4.2 scheme).
                    //   Matches the uint8 codebook indices in the weight blobs.
    Fp8Wmma,        // RDNA4+: native FP8 wave-matrix (future)
    Fp16Fallback,   // FP16×FP16→FLOAT32 cooperative matrix (verified available)
};

// The concrete cooperative-matrix mode the device exposes for the selected
// profile. The backend's GLSL shaders use GLSL_cooperative_matrix with these
// exact component types.
struct CoopMatrixMode {
    VkComponentTypeKHR aType = VK_COMPONENT_TYPE_MAX_ENUM_KHR;
    VkComponentTypeKHR bType = VK_COMPONENT_TYPE_MAX_ENUM_KHR;
    VkComponentTypeKHR cType = VK_COMPONENT_TYPE_MAX_ENUM_KHR; // accumulator
    VkComponentTypeKHR resultType = VK_COMPONENT_TYPE_MAX_ENUM_KHR;
    VkExtent2D tile = {0, 0};
    bool saturatingAccumulation = false;
};

struct GpuCapability {
    bool valid = false;
    Fsr4Profile profile = Fsr4Profile::Unsupported;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::string deviceName;
    std::string driverName;
    std::string driverInfo;
    uint32_t apiVersion = 0;
    bool isRdna3 = false;
    bool isRdna4 = false;
    bool hasCooperativeMatrix = false;
    // RDNA3 INT8 path: SINT8xSINT8->SINT32 symmetric quantized matrix math.
    bool hasUint8Input = false;
    bool hasInt32Accum = false;
    // FP16 fallback path: FLOAT16×FLOAT16→FLOAT32.
    bool hasFp16Fallback = false;
    CoopMatrixMode int8Mode;     // resolved when profile == Int8Dot4
    CoopMatrixMode fp16Mode;     // resolved when profile == Fp16Fallback
    VkExtent2D cooperativeMatrixTileSize = {0, 0};
    std::string failReason;
};

class GpuCapabilityProbe {
public:
    // Auto-pick the best AMD device in the instance and probe it.
    static GpuCapability probe(VkInstance instance);
    // Probe a specific physical device. Pass the instance so the cooperative-
    // matrix extension functions can be resolved.
    static GpuCapability probe(VkPhysicalDevice device, VkInstance instanceForLookup);

    [[nodiscard]] static const char* profileName(Fsr4Profile p);
    [[nodiscard]] static bool profileViable(Fsr4Profile p) {
        return p != Fsr4Profile::Unsupported;
    }

private:
    static bool detectInt8CooperativeMatrix(VkPhysicalDevice device,
                                            VkInstance instanceForLookup,
                                            GpuCapability& out);
};

} // namespace temporal_forge
