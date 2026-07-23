// UpscaleTypes.cpp
#include "UpscaleTypes.hpp"

namespace temporal_forge {

const char* presetDisplayName(UpscalePreset p) {
    switch (p) {
        case UpscalePreset::Off:              return "Off";
        case UpscalePreset::NativeAA:         return "NativeAA 1.0x";
        case UpscalePreset::Quality:          return "Quality 1.5x";
        case UpscalePreset::Balanced:         return "Balanced 1.7x";
        case UpscalePreset::Performance:      return "Performance 2.0x";
        case UpscalePreset::UltraPerformance: return "Ultra Performance 3.0x";
        case UpscalePreset::AutoMatchDisplay: return "Auto Match Display";
    }
    return "Unknown";
}

// spec 02 section: FSR Preset Ratios
float presetRatio(UpscalePreset p) {
    switch (p) {
        case UpscalePreset::NativeAA:         return 1.0f;
        case UpscalePreset::Quality:          return 1.5f;
        case UpscalePreset::Balanced:         return 1.7f;
        case UpscalePreset::Performance:      return 2.0f;
        case UpscalePreset::UltraPerformance: return 3.0f;
        // Off and AutoMatchDisplay have no fixed ratio; AutoMatchDisplay is
        // resolved by FsrTargetMath against the active display before use.
        case UpscalePreset::Off:
        case UpscalePreset::AutoMatchDisplay:
            return 1.0f;
    }
    return 1.0f;
}

bool presetIsTemporal(UpscalePreset p) {
    switch (p) {
        case UpscalePreset::Off:
            return false;
        case UpscalePreset::NativeAA:
        case UpscalePreset::Quality:
        case UpscalePreset::Balanced:
        case UpscalePreset::Performance:
        case UpscalePreset::UltraPerformance:
        case UpscalePreset::AutoMatchDisplay:
            return true;
    }
    return false;
}

const char* backendDisplayName(BackendKind b) {
    switch (b) {
        case BackendKind::Fsr23Sdk:           return "FSR 2.3 SDK";
        case BackendKind::Fsr4ReExperimental: return "FSR4-RE Experimental";
        case BackendKind::SpatialFallback:    return "Spatial fallback";
        case BackendKind::Null:               return "Null";
    }
    return "Unknown";
}

const char* upscaleErrorName(UpscaleError e) {
    switch (e) {
        case UpscaleError::None:                  return "None";
        case UpscaleError::UnsupportedDevice:     return "UnsupportedDevice";
        case UpscaleError::UnsupportedFormat:     return "UnsupportedFormat";
        case UpscaleError::ContextCreateFailed:   return "ContextCreateFailed";
        case UpscaleError::DispatchFailed:        return "DispatchFailed";
        case UpscaleError::InvalidResource:       return "InvalidResource";
        case UpscaleError::ShaderCompileFailed:   return "ShaderCompileFailed";
        case UpscaleError::WeightValidationFailed:return "WeightValidationFailed";
        case UpscaleError::OutOfMemory:           return "OutOfMemory";
        case UpscaleError::InternalError:         return "InternalError";
    }
    return "Unknown";
}

} // namespace temporal_forge
