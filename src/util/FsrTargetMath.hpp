// FsrTargetMath.hpp — spec 02 section "Target Size Calculation".
//
//   fsrTargetWidth  = align_even(round(sourceWidth  * presetRatio));
//   fsrTargetHeight = align_even(round(sourceHeight * presetRatio));
//
// Two-stage scaling model (spec 02):
//   source frame
//     -> FSR preset reconstruction target   (this file computes this)
//     -> final presentation scale to window (handled by PresentScaler)
//
// The FSR target depends ONLY on source size and preset. It is independent
// of window/fullscreen size. Window resize must never change the FSR target
// and never recreate the FSR context (spec 02, 07, 08 risk #8).
#pragma once
#include "backend/UpscaleTypes.hpp"
#include "util/Align.hpp"
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace temporal_forge {

struct Size2D {
    uint32_t width;
    uint32_t height;
};

// fsrProgressivePassSizes: build a progressive chain where each pass grows the
//                          previous input by 1.5x, capped at the requested target.
//
// Called by: PlaybackEngine::initFsr4Path() to decide whether the FSR4 path runs
//            in one shot or chains multiple progressive passes between source and
//            target (needed when the ratio is large).
// Calls:     alignEven.
// Notes:     The last pass is capped at the requested target and never overshoots
//            it. Returns an empty vector for zero-sized source/target.
inline std::vector<Size2D> fsrProgressivePassSizes(uint32_t sourceWidth,
                                                    uint32_t sourceHeight,
                                                    uint32_t targetWidth,
                                                    uint32_t targetHeight) {
    std::vector<Size2D> passes;
    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 ||
        targetHeight == 0)
        return passes;

    Size2D current{alignEven(sourceWidth), alignEven(sourceHeight)};
    const Size2D target{alignEven(targetWidth), alignEven(targetHeight)};
    while (current.width < target.width || current.height < target.height) {
        const Size2D next{
            std::min(target.width, alignEven(static_cast<uint32_t>(
                                      std::ceil(current.width * 1.5f)))),
            std::min(target.height, alignEven(static_cast<uint32_t>(
                                      std::ceil(current.height * 1.5f))))};
        if (next.width == current.width && next.height == current.height)
            break;
        passes.push_back(next);
        current = next;
    }
    return passes;
}

// nativeInt8UltraPerformanceTarget: returns the fixed 1920x1080 / 3840x2160
//                                   target the INT8 native-i8 shader packs ship in.
//
// Called by: PlaybackEngine::initFsr4Path() and FsrController to detect the
//            "native INT8 ultra-performance" preset and pick the matching shader
//            pack instead of a generated one.
// Notes:     Returns {0,0} (meaning "no native pack applies") when the source is
//            empty, taller than 1080, or not 16:9 within ~1% tolerance — because
//            the prebuilt packs are only generated for 16:9 sources up to 1080p.
inline Size2D nativeInt8UltraPerformanceTarget(uint32_t sourceWidth,
                                                uint32_t sourceHeight) {
    if (sourceWidth == 0 || sourceHeight == 0 || sourceHeight > 1080)
        return {0, 0};

    const uint64_t wide = static_cast<uint64_t>(sourceWidth) * 9u;
    const uint64_t tall = static_cast<uint64_t>(sourceHeight) * 16u;
    const uint64_t aspectError = wide > tall ? wide - tall : tall - wide;
    if (aspectError * 100u > tall)
        return {0, 0};

    return sourceHeight <= 360 ? Size2D{1920, 1080} : Size2D{3840, 2160};
}

inline Size2D nativeInt8FourThreeTarget(uint32_t sourceWidth,
                                        uint32_t sourceHeight) {
    if (sourceWidth == 0 || sourceHeight == 0 || sourceHeight > 1080)
        return {0, 0};
    const uint64_t wide = static_cast<uint64_t>(sourceWidth) * 3u;
    const uint64_t tall = static_cast<uint64_t>(sourceHeight) * 4u;
    const uint64_t aspectError = wide > tall ? wide - tall : tall - wide;
    if (aspectError * 100u > tall)
        return {0, 0};
    return sourceHeight <= 540 ? Size2D{1440, 1080} : Size2D{2880, 2160};
}

inline Size2D nativeInt8FixedTarget(uint32_t sourceWidth,
                                    uint32_t sourceHeight) {
    const Size2D wide = nativeInt8UltraPerformanceTarget(sourceWidth,
                                                         sourceHeight);
    return wide.width != 0 ? wide
                           : nativeInt8FourThreeTarget(sourceWidth, sourceHeight);
}

// Fixed native INT8 graphs are generated for 16:9 tensors. Keep this policy
// reusable at every selection boundary, including callers that construct a
// dispatch harness directly instead of going through PlaybackEngine.
inline bool nativeInt8SourceAspectSupported(uint32_t sourceWidth,
                                             uint32_t sourceHeight) {
    return nativeInt8FixedTarget(sourceWidth, sourceHeight).width != 0;
}

struct PresetInfo {
    UpscalePreset preset;
    const char* displayName;
    float ratio;
    bool temporal;
};

// spec 04 section 6: preset table (Off has no temporal FSR ratio)
constexpr PresetInfo kPresetTable[] = {
    {UpscalePreset::Off,              "Off",                   0.0f, false},
    {UpscalePreset::NativeAA,         "NativeAA 1.0x",         1.0f, true},
    {UpscalePreset::Quality,          "Quality 1.5x",          1.5f, true},
    {UpscalePreset::Balanced,         "Balanced 1.7x",         1.7f, true},
    {UpscalePreset::Performance,      "Performance 2.0x",      2.0f, true},
    {UpscalePreset::UltraPerformance, "Ultra Performance 3.0x",3.0f, true},
};
constexpr size_t kPresetTableSize = std::size(kPresetTable);

// findPresetInfo: looks up the display name, ratio, and temporal flag for a preset.
//
// Called by: FsrController (preset labels + options list) and anywhere preset
//            metadata is needed.
// Notes:     Returns nullptr for an out-of-range preset value.
inline const PresetInfo* findPresetInfo(UpscalePreset p) {
    for (size_t i = 0; i < kPresetTableSize; ++i)
        if (kPresetTable[i].preset == p) return &kPresetTable[i];
    return nullptr;
}

// fsrTargetSize: spec 02 target size for a fixed preset.
//
// Called by: FsrController::refreshSource/presetLabel/chainLabel/presetOptions
//            (to display "Performance 2.0x — 1920x1080 -> 3840x2160"), and by
//            autoMatchPreset below.
// Calls:     presetRatio (backend/UpscaleTypes), alignEven, alignTo.
// Notes:     Off / Spatial presets pass through with no FSR reconstruction.
//            Default alignment is even (2px, the minimum); callers requiring 8px
//            (preferred) or backend-specific alignment pass a larger value.
inline Size2D fsrTargetSize(uint32_t sourceWidth, uint32_t sourceHeight,
                            UpscalePreset preset, uint32_t alignment = 2) {
    // Off / Spatial: no FSR reconstruction.
    if (preset == UpscalePreset::Off) {
        return Size2D{alignEven(sourceWidth), alignEven(sourceHeight)};
    }
    const float ratio = presetRatio(preset);
    const float w = std::round(static_cast<float>(sourceWidth)  * ratio);
    const float h = std::round(static_cast<float>(sourceHeight) * ratio);
    uint32_t tw = alignEven(static_cast<uint32_t>(w));
    uint32_t th = alignEven(static_cast<uint32_t>(h));
    if (alignment > 2) {
        tw = alignTo(tw, alignment);
        th = alignTo(th, alignment);
    }
    return Size2D{tw, th};
}

// autoMatchPreset: spec 02 "Auto-Match Display" — choose the nearest preset whose
//                  target size reaches the display, given the source.
//
// Called by: resolvePreset when the user picks AutoMatchDisplay.
// Calls:     fsrTargetSize.
// Notes:     Ties prefer the smaller preset (less GPU cost). Manual user choice
//            always wins over this. Examples (spec 02):
//              1920x1080 -> 3840x2160 : Performance 2.0x
//              1280x720  -> 2560x1440 : Performance 2.0x
//              1280x720  -> 3840x2160 : Ultra Performance 3.0x
inline UpscalePreset autoMatchPreset(uint32_t sourceWidth, uint32_t sourceHeight,
                                     uint32_t displayWidth, uint32_t displayHeight) {
    // Candidate ratios in ascending order of cost.
    constexpr UpscalePreset candidates[] = {
        UpscalePreset::NativeAA,
        UpscalePreset::Quality,
        UpscalePreset::Balanced,
        UpscalePreset::Performance,
        UpscalePreset::UltraPerformance,
    };
    const uint64_t displayArea =
        static_cast<uint64_t>(displayWidth) * static_cast<uint64_t>(displayHeight);
    UpscalePreset chosen = UpscalePreset::NativeAA;
    for (UpscalePreset c : candidates) {
        const Size2D t = fsrTargetSize(sourceWidth, sourceHeight, c, 2);
        const uint64_t area =
            static_cast<uint64_t>(t.width) * static_cast<uint64_t>(t.height);
        if (area >= displayArea) {
            chosen = c;
            break; // first candidate that reaches display is the cheapest fit
        }
        chosen = c; // keep raising toward the largest available
    }
    return chosen;
}

// resolvePreset: resolve AutoMatchDisplay to a concrete preset (identity otherwise).
//
// Called by: preset-selection entry points that accept the AutoMatchDisplay
//            sentinel and need a real preset for the rest of the pipeline.
// Calls:     autoMatchPreset.
inline UpscalePreset resolvePreset(UpscalePreset preset,
                                   uint32_t sourceWidth, uint32_t sourceHeight,
                                   uint32_t displayWidth, uint32_t displayHeight) {
    if (preset == UpscalePreset::AutoMatchDisplay) {
        return autoMatchPreset(sourceWidth, sourceHeight, displayWidth, displayHeight);
    }
    return preset;
}

} // namespace temporal_forge
