// SettingsStore.cpp — minimal hand-rolled JSON. No external dep.
// The schema is small and stable; a 60-line reader/writer beats pulling in
// a JSON library for the MVP.
#include "config/SettingsStore.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace temporal_forge {

namespace {

// --- tiny JSON value extractor (string-view based, schema-specific) ---

std::string extractString(std::string_view body, std::string_view key) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return {};
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return {};
    auto q1 = body.find('"', colon);
    if (q1 == std::string_view::npos) return {};
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return {};
    return std::string(body.substr(q1 + 1, q2 - q1 - 1));
}

long extractInt(std::string_view body, std::string_view key, long fallback) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return fallback;
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return fallback;
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    bool neg = false;
    if (i < body.size() && body[i] == '-') { neg = true; ++i; }
    long v = 0; bool any = false;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + (body[i] - '0'); ++i; any = true;
    }
    if (!any) return fallback;
    return neg ? -v : v;
}

double extractDouble(std::string_view body, std::string_view key, double fallback) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return fallback;
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return fallback;
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    bool neg = false;
    if (i < body.size() && body[i] == '-') { neg = true; ++i; }
    double v = 0; bool any = false;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + (body[i] - '0'); ++i; any = true;
    }
    if (i < body.size() && body[i] == '.') {
        ++i; double f = 0.1;
        while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
            v += (body[i] - '0') * f; f *= 0.1; ++i; any = true;
        }
    }
    if (!any) return fallback;
    return neg ? -v : v;
}

bool extractBool(std::string_view body, std::string_view key, bool fallback) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    auto k = body.find(needle);
    if (k == std::string_view::npos) return fallback;
    auto colon = body.find(':', k);
    if (colon == std::string_view::npos) return fallback;
    auto t = body.find("true", colon);
    auto f = body.find("false", colon);
    if (t != std::string_view::npos && (f == std::string_view::npos || t < f))
        return true;
    if (f != std::string_view::npos) return false;
    return fallback;
}

BackendKind parseBackend(std::string_view s) {
    if (s == "Fsr23Sdk") return BackendKind::Fsr23Sdk;
    if (s == "Fsr4ReExperimental") return BackendKind::Fsr4ReExperimental;
    if (s == "SpatialFallback") return BackendKind::SpatialFallback;
    return BackendKind::Fsr23Sdk;
}
const char* backendKey(BackendKind b) {
    switch (b) {
        case BackendKind::Fsr23Sdk: return "Fsr23Sdk";
        case BackendKind::Fsr4ReExperimental: return "Fsr4ReExperimental";
        case BackendKind::SpatialFallback: return "SpatialFallback";
        case BackendKind::Null: return "Fsr23Sdk"; // never persist Null
    }
    return "Fsr23Sdk";
}
UpscalePreset parsePreset(std::string_view s) {
    if (s == "Off") return UpscalePreset::Off;
    if (s == "NativeAA") return UpscalePreset::NativeAA;
    if (s == "Quality") return UpscalePreset::Quality;
    if (s == "Balanced") return UpscalePreset::Balanced;
    if (s == "Performance") return UpscalePreset::Performance;
    if (s == "UltraPerformance") return UpscalePreset::UltraPerformance;
    if (s == "AutoMatchDisplay") return UpscalePreset::AutoMatchDisplay;
    return UpscalePreset::Quality;
}
const char* presetKey(UpscalePreset p) {
    switch (p) {
        case UpscalePreset::Off: return "Off";
        case UpscalePreset::NativeAA: return "NativeAA";
        case UpscalePreset::Quality: return "Quality";
        case UpscalePreset::Balanced: return "Balanced";
        case UpscalePreset::Performance: return "Performance";
        case UpscalePreset::UltraPerformance: return "UltraPerformance";
        case UpscalePreset::AutoMatchDisplay: return "AutoMatchDisplay";
    }
    return "Quality";
}
MotionMode parseMotion(std::string_view s) {
    if (s == "Codec") return MotionMode::Codec;
    if (s == "Block") return MotionMode::Block;
    if (s == "Zero") return MotionMode::Zero;
    return MotionMode::AutoCheap;
}
const char* motionKey(MotionMode m) {
    switch (m) {
        case MotionMode::AutoCheap: return "AutoCheap";
        case MotionMode::Codec: return "Codec";
        case MotionMode::Block: return "Block";
        case MotionMode::Zero: return "Zero";
    }
    return "AutoCheap";
}
DepthMode parseDepth(std::string_view s) {
    return s == "EdgeLite" ? DepthMode::EdgeLite : DepthMode::Flat;
}
const char* depthKey(DepthMode d) {
    return d == DepthMode::EdgeLite ? "EdgeLite" : "Flat";
}
ReactiveMode parseReactive(std::string_view s) {
    if (s == "Off") return ReactiveMode::Off;
    if (s == "Aggressive") return ReactiveMode::Aggressive;
    return ReactiveMode::CheapAuto;
}
const char* reactiveKey(ReactiveMode r) {
    switch (r) {
        case ReactiveMode::Off: return "Off";
        case ReactiveMode::CheapAuto: return "CheapAuto";
        case ReactiveMode::Aggressive: return "Aggressive";
    }
    return "CheapAuto";
}
PresentationScaler parsePresentation(std::string_view s) {
    if (s == "Bilinear") return PresentationScaler::Bilinear;
    if (s == "Bicubic") return PresentationScaler::Bicubic;
    if (s == "Lanczos") return PresentationScaler::Lanczos;
    if (s == "Easu") return PresentationScaler::Easu;
    // Auto and unknown/legacy values now select the quality-first default.
    return PresentationScaler::Bicubic;
}
const char* presentationKey(PresentationScaler p) {
    switch (p) {
        case PresentationScaler::Auto: return "Auto";
        case PresentationScaler::Bilinear: return "Bilinear";
        case PresentationScaler::Bicubic: return "Bicubic";
        case PresentationScaler::Lanczos: return "Lanczos";
        case PresentationScaler::Easu: return "Easu";
    }
    return "Bicubic";
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}

bool SettingsStore::load(Settings& out) {
    std::ifstream f(path_);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();
    std::string_view v(body);

    out.allowExperimentalAsDefault = extractBool(v, "allowExperimentalAsDefault", true);
    out.backend = parseBackend(extractString(v, "backend"));
    // Legacy configs may still carry this guard. Keep honoring an explicit
    // false value, but default new/missing values to allowing FSR4 selection.
    if (out.backend == BackendKind::Fsr4ReExperimental && !out.allowExperimentalAsDefault)
        out.backend = BackendKind::Fsr23Sdk;
    out.preset = parsePreset(extractString(v, "preset"));
    out.sharpness = static_cast<float>(extractDouble(v, "sharpness", 0.3));
    out.jitterStrength = static_cast<float>(extractDouble(v, "jitterStrength", 1.0));
    out.motionMode = parseMotion(extractString(v, "motionMode"));
    out.depthMode = parseDepth(extractString(v, "depthMode"));
    out.reactiveMode = parseReactive(extractString(v, "reactiveMode"));
    out.presentationScaler = parsePresentation(extractString(v, "presentationScaler"));
    // Older files may explicitly contain Auto. Keep the file readable while
    // making the new default deterministic after the next save.
    if (out.presentationScaler == PresentationScaler::Auto)
        out.presentationScaler = PresentationScaler::Bicubic;
    out.brightness = static_cast<float>(extractDouble(v, "brightness", 0.0));
    out.contrast = static_cast<float>(extractDouble(v, "contrast", 0.0));
    out.saturation = static_cast<float>(extractDouble(v, "saturation", 0.0));
    out.hue = static_cast<float>(extractDouble(v, "hue", 0.0));
    out.gamma = static_cast<float>(extractDouble(v, "gamma", 1.0));
    out.audioTrack = static_cast<int>(extractInt(v, "audioTrack", -1));
    out.subtitleTrack = static_cast<int>(extractInt(v, "subtitleTrack", -2));
    out.volume = static_cast<int>(extractInt(v, "volume", 100));
    out.windowX = static_cast<int>(extractInt(v, "windowX", 100));
    out.windowY = static_cast<int>(extractInt(v, "windowY", 100));
    out.windowW = static_cast<int>(extractInt(v, "windowW", 1280));
    out.windowH = static_cast<int>(extractInt(v, "windowH", 720));
    out.fullscreen = extractBool(v, "fullscreen", false);
    out.lastOpenDir = extractString(v, "lastOpenDir");
    return true;
}

void SettingsStore::save(const Settings& s) {
    std::filesystem::create_directories(path_.parent_path());
    const auto tmp = path_.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) { logError("SettingsStore: cannot write {}", tmp); return; }
        f << "{\n";
        f << "  \"backend\": \"" << backendKey(s.backend) << "\",\n";
        f << "  \"preset\": \"" << presetKey(s.preset) << "\",\n";
        f << "  \"sharpness\": " << s.sharpness << ",\n";
        f << "  \"jitterStrength\": " << s.jitterStrength << ",\n";
        f << "  \"motionMode\": \"" << motionKey(s.motionMode) << "\",\n";
        f << "  \"depthMode\": \"" << depthKey(s.depthMode) << "\",\n";
        f << "  \"reactiveMode\": \"" << reactiveKey(s.reactiveMode) << "\",\n";
        f << "  \"presentationScaler\": \"" << presentationKey(s.presentationScaler) << "\",\n";
        f << "  \"brightness\": " << s.brightness << ",\n";
        f << "  \"contrast\": " << s.contrast << ",\n";
        f << "  \"saturation\": " << s.saturation << ",\n";
        f << "  \"hue\": " << s.hue << ",\n";
        f << "  \"gamma\": " << s.gamma << ",\n";
        f << "  \"audioTrack\": " << s.audioTrack << ",\n";
        f << "  \"subtitleTrack\": " << s.subtitleTrack << ",\n";
        f << "  \"volume\": " << s.volume << ",\n";
        f << "  \"windowX\": " << s.windowX << ",\n";
        f << "  \"windowY\": " << s.windowY << ",\n";
        f << "  \"windowW\": " << s.windowW << ",\n";
        f << "  \"windowH\": " << s.windowH << ",\n";
        f << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n";
        f << "  \"lastOpenDir\": \"" << s.lastOpenDir << "\",\n";
        f << "  \"allowExperimentalAsDefault\": "
          << (s.allowExperimentalAsDefault ? "true" : "false") << "\n";
        f << "}\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) logError("SettingsStore: rename failed: {}", ec.message());
}

std::filesystem::path SettingsStore::defaultPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "temporal-forge-player" / "settings.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "temporal-forge-player" / "settings.json";
    return std::filesystem::path("temporal-forge-player-settings.json");
}

std::string_view motionModeName(MotionMode m) {
    switch (m) {
        case MotionMode::AutoCheap: return "Auto cheap";
        case MotionMode::Codec:     return "Codec vectors";
        case MotionMode::Block:     return "Block vectors";
        case MotionMode::Zero:      return "Zero vectors";
    }
    return "Auto cheap";
}
std::string_view depthModeName(DepthMode d) {
    return d == DepthMode::EdgeLite ? "Edge-lite" : "Flat";
}
std::string_view reactiveModeName(ReactiveMode r) {
    switch (r) {
        case ReactiveMode::Off:        return "Off";
        case ReactiveMode::CheapAuto:  return "Cheap Auto";
        case ReactiveMode::Aggressive: return "Aggressive";
    }
    return "Cheap Auto";
}
std::string_view presentationScalerName(PresentationScaler p) {
    switch (p) {
        case PresentationScaler::Auto:     return "Auto";
        case PresentationScaler::Bilinear: return "Bilinear";
        case PresentationScaler::Bicubic:  return "Bicubic";
        case PresentationScaler::Lanczos:  return "Lanczos";
        case PresentationScaler::Easu:     return "EASU-style";
    }
    return "Auto";
}

} // namespace temporal_forge
