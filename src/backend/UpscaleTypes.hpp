// UpscaleTypes.hpp
// Backend contract types — verbatim from spec docs 03/04.
// Every backend (FSR2.3, FSR4-RE experimental, spatial fallback, null)
// receives the same high-level video reconstruction packet. The player
// does not care which backend is active.
//
// Inviolable rule (spec 00/02):
//   1 decoded input frame -> 1 upscaled output frame
//   same timestamps, same frame count, same source frame rate
// No backend may change frame rate, generate frames, or own subtitle/UI.
#pragma once

#include <cstdint>

namespace temporal_forge {

// spec 04 section 6
enum class UpscalePreset : uint8_t {
    Off,
    NativeAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
    AutoMatchDisplay,
};

enum class BackendKind : uint8_t {
    Null,
    SpatialFallback,
    Fsr23Sdk,
    Fsr4ReExperimental,
};

enum class UpscaleError : uint8_t {
    None,
    UnsupportedDevice,
    UnsupportedFormat,
    ContextCreateFailed,
    DispatchFailed,
    InvalidResource,
    ShaderCompileFailed,
    WeightValidationFailed,
    OutOfMemory,
    InternalError,
};

// Backend-facing handle to a GPU image. Concrete Vulkan image/extent are
// owned by the render layer; backends only carry them opaquely.
struct GpuTexture {
    uint64_t handle = 0;          // VkImage cast through uint64_t for ABI cleanliness
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;          // VkFormat
    uint32_t mipCount = 1;
    bool owned = false;           // backend never owns these
};

// spec 04 section: UpscaleContextDesc
struct UpscaleContextDesc {
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t fsrOutputWidth = 0;
    uint32_t fsrOutputHeight = 0;

    bool hdr = false;
    bool nonlinearColor = true;
    bool enableAutoExposure = true;
    bool enableSharpening = true;

    float sharpness = 0.2f;

    UpscalePreset preset = UpscalePreset::Off;
    BackendKind backendKind = BackendKind::Fsr23Sdk;
};

// spec 03 section 9: cheapest MVP packet
struct VideoFsrPacket {
    GpuTexture color;        // jittered current RGB frame
    GpuTexture motion;       // codec/block/zero vectors (RG16F)
    GpuTexture depth;        // flat or edge-lite compatibility depth (R32F)
    GpuTexture reactive;     // cheap luma/confidence mask (R8)
    GpuTexture tcMask;       // cleared or fake (R8)
    GpuTexture exposure;     // null/auto or 1x1 (R16F)

    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t fsrOutputWidth = 0;
    uint32_t fsrOutputHeight = 0;

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float frameTimeMs = 16.6667f;

    bool reset = false;
    bool hdr = false;
};

// spec 04 section: UpscaleOutputPacket
struct UpscaleOutputPacket {
    GpuTexture output;
    uint32_t width = 0;
    uint32_t height = 0;
    bool historyValid = false;
};

// spec 04 section: per-backend telemetry
struct BackendInfo {
    const char* name = "";
    const char* version = "";
    BackendKind backendKind = BackendKind::Null;
    UpscalePreset activePreset = UpscalePreset::Off;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t fsrOutputWidth = 0;
    uint32_t fsrOutputHeight = 0;
    double lastDispatchMs = 0.0;
    bool historyValid = false;
    uint64_t resetCount = 0;
    uint64_t gpuMemoryEstimate = 0;
    UpscaleError lastError = UpscaleError::None;
};

const char* presetDisplayName(UpscalePreset p);
float presetRatio(UpscalePreset p);
bool presetIsTemporal(UpscalePreset p);
const char* backendDisplayName(BackendKind b);
const char* upscaleErrorName(UpscaleError e);

} // namespace temporal_forge
