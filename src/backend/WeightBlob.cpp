// WeightBlob.cpp
#include "backend/WeightBlob.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace temporal_forge {

namespace {

struct PresetInfo {
    Fsr4Preset preset;
    const char* name;
    const char* file;
    bool drs;
};
constexpr PresetInfo kPresets[] = {
    {Fsr4Preset::Quality,     "Quality",          "quality.bin",    false},
    {Fsr4Preset::Balanced,    "Balanced",         "balanced.bin",   false},
    {Fsr4Preset::Performance, "Performance",      "performance.bin",false},
    {Fsr4Preset::UltraPerf,   "Ultra Performance","ultraperf.bin",  false},
    {Fsr4Preset::Native,      "Native",           "native.bin",     false},
    {Fsr4Preset::Drs,         "DRS",              "drs.bin",        true},
};

const PresetInfo* findPreset(Fsr4Preset p) {
    for (const auto& info : kPresets)
        if (info.preset == p) return &info;
    return nullptr;
}

} // namespace

const char* WeightBlobLoader::presetName(Fsr4Preset p) {
    if (const auto* info = findPreset(p)) return info->name;
    return "Unknown";
}
const char* WeightBlobLoader::presetFileName(Fsr4Preset p) {
    if (const auto* info = findPreset(p)) return info->file;
    return "unknown.bin";
}
bool WeightBlobLoader::presetIsDrs(Fsr4Preset p) {
    if (const auto* info = findPreset(p)) return info->drs;
    return false;
}

uint64_t WeightBlobLoader::fnv1a64(const uint8_t* data, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

Fsr4BlobLoadResult WeightBlobLoader::load(Fsr4Preset preset, const std::string& path) {
    Fsr4BlobLoadResult r;
    const auto* info = findPreset(preset);
    if (!info) { r.failReason = "unknown preset"; return r; }
    r.meta.preset = preset;
    r.meta.name = info->name;
    r.meta.isDrs = info->drs;
    r.meta.isStandardShared = !info->drs;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { r.failReason = "cannot open: " + path; return r; }
    const auto sz = static_cast<size_t>(f.tellg());
    if (sz != kFsr4BlobSize) {
        r.failReason = "bad size: got " + std::to_string(sz) +
                       " expected " + std::to_string(kFsr4BlobSize);
        return r;
    }
    f.seekg(0);
    r.data.resize(kFsr4BlobSize);
    f.read(reinterpret_cast<char*>(r.data.data()), kFsr4BlobSize);
    if (!f) { r.failReason = "short read"; return r; }

    // Padding zone (last 96 B) must be all zero.
    const uint8_t* pad = r.data.data() + kFsr4PadZoneOffset;
    for (size_t i = 0; i < kFsr4PadZoneSize; ++i) {
        if (pad[i] != 0) {
            r.failReason = "padding zone not zero at offset " +
                           std::to_string(kFsr4PadZoneOffset + i);
            return r;
        }
    }

    // Unique-uint8 census over the v4.1 tensor-data region. v4.1.0 expects
    // 255 unique values (full uint8 range); v4.0.2 had 122. This distinguishes
    // a real 4.1.0 blob from a 4.0.2 one without interpreting tensor offsets.
    const uint8_t* w = r.data.data() + kFsr4WeightZoneOffset;
    std::unordered_set<unsigned> seen;
    seen.reserve(256);
    for (size_t i = 0; i < kFsr4WeightZoneSize; ++i) seen.insert(w[i]);
    r.meta.uniqueUint8Values = static_cast<uint32_t>(seen.size());
    if (r.meta.uniqueUint8Values != 255) {
        logWarn("WeightBlob: {} has {} unique uint8 weights (v4.1.0 expects 255)",
                r.meta.name, r.meta.uniqueUint8Values);
    }

    r.ok = true;
    logInfo("WeightBlob: loaded {} ({} B, {} unique uint8, drs={})",
            r.meta.name, sz, r.meta.uniqueUint8Values, r.meta.isDrs);
    return r;
}

Fsr4BlobView WeightBlobLoader::view(const Fsr4BlobLoadResult& loaded) {
    Fsr4BlobView v;
    if (!loaded.ok || loaded.data.empty()) return v;
    v.bytes = loaded.data.data();
    v.biasesFp16 = reinterpret_cast<const uint16_t*>(v.bytes + kFsr4BiasZoneOffset);
    v.weightsUint8 = v.bytes + kFsr4WeightZoneOffset;
    v.scalesFp16 = reinterpret_cast<const uint16_t*>(v.bytes + kFsr4ScaleZoneOffset);
    v.padding = v.bytes + kFsr4PadZoneOffset;
    return v;
}

} // namespace temporal_forge
