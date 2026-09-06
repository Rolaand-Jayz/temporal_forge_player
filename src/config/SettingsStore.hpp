// SettingsStore.hpp — spec 05 "Settings Persistence".
//
// Persisted (spec 05): last backend, last preset, motion mode, depth mode,
// reactive mode, sharpness, presentation scaler, subtitle/audio prefs, last
// open directory, window geometry.
//
// The RDNA3/Linux FSR4-RE path is exposed as a selectable startup backend so
// it can be exercised from the app instead of remaining dormant.
#pragma once
#include "backend/UpscaleTypes.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace temporal_forge {

enum class MotionMode : uint8_t {
    AutoCheap,   // codec MVs -> block match repair -> zero fill (spec 03 default)
    Codec,       // codec block MVs only
    Block,       // GPU block matching only
    Zero,        // zero vectors
};
enum class DepthMode : uint8_t { Flat, EdgeLite };
enum class ReactiveMode : uint8_t { Off, CheapAuto, Aggressive };
enum class PresentationScaler : uint8_t {
    Auto, Bilinear, Bicubic, Lanczos, Easu
};

struct Settings {
    // Upscale
    BackendKind backend = BackendKind::Fsr4ReExperimental;
    UpscalePreset preset = UpscalePreset::Quality;
    float sharpness = 0.3f;
    float jitterStrength = 1.0f;
    // Side buffers (spec 03 MVP defaults)
    MotionMode motionMode = MotionMode::AutoCheap;
    DepthMode depthMode = DepthMode::Flat;
    ReactiveMode reactiveMode = ReactiveMode::CheapAuto;
    // Bicubic is the quality-first default. Auto remains in the enum for
    // compatibility with older settings files and is normalized to Bicubic
    // when loaded.
    PresentationScaler presentationScaler = PresentationScaler::Bicubic;
    // Color adjustments (applied at YUV→RGBA conversion in VideoSurfaceItem).
    // All normalized: brightness/contrast/saturation/hue in [-1, 1], gamma in [0.1, 3.0].
    float brightness = 0.0f;
    float contrast = 0.0f;
    float saturation = 0.0f;
    float hue = 0.0f;
    float gamma = 1.0f;
    // Media
    int audioTrack = -1;       // -1 = default stream
    int subtitleTrack = -2;    // -2 = none, -1 = default stream
    int volume = 100;          // 0..100
    // Window
    int windowX = 100, windowY = 100;
    int windowW = 1280, windowH = 720;
    bool fullscreen = false;
    std::string lastOpenDir;
    // Experimental
    bool allowExperimentalAsDefault = true;
};

class SettingsStore {
public:
    // SettingsStore ctor: remember the on-disk path for load/save.
    //                     Called by main.cpp with defaultPath() (or an env override).
    explicit SettingsStore(std::filesystem::path path);

    // load: read settings.json from disk into out.
    //
    // Called by: main.cpp at startup (loads persisted user prefs before building
    //            the QML controllers).
    // Returns:   true if the file existed and parsed; false leaves out partially
    //            defaulted (callers fall back to the in-struct defaults).
    // Notes:     Legacy configs may carry an allowExperimentalAsDefault guard;
    //            honoring an explicit false downgrades Fsr4ReExperimental to
    //            Fsr23Sdk. Uses tiny hand-rolled JSON extractors (no dep added).
    bool load(Settings& out);

    // save: persist settings atomically (temp file + rename).
    //
    // Called by: main.cpp (after loading, to normalize the file) and the QML
    //            controllers whenever a setting changes.
    // Calls:     create_directories, the *Key() name accessors, filesystem::rename.
    void save(const Settings& s);

    // path: the on-disk settings file path this store reads/writes.
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // defaultPath: resolves the default settings location.
    //
    // Resolves $XDG_CONFIG_HOME/temporal-forge-player/settings.json, falling back
    // to ~/.config/... when XDG_CONFIG_HOME is unset, then to a relative file
    // when HOME is also unset. Called by main.cpp at startup.
    static std::filesystem::path defaultPath();

private:
    std::filesystem::path path_;
};

// motionModeName / depthModeName / reactiveModeName / presentationScalerName:
// human-readable names for each enum, shared by the QML bridges (option lists)
// and the persistence layer. Return a sensible default for out-of-range values.
std::string_view motionModeName(MotionMode m);
std::string_view depthModeName(DepthMode d);
std::string_view reactiveModeName(ReactiveMode r);
std::string_view presentationScalerName(PresentationScaler p);

} // namespace temporal_forge
