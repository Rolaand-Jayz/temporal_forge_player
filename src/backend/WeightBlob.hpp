// WeightBlob.hpp — FSR 4.1.0 RE weight-blob loader + validator.
//
// Loads the 6 × 131,072-byte neural-network weight blobs extracted by the
// FSR 4.1.0 RE project. The RE is an unvalidated evidence set, so this loader
// only validates local blob invariants and candidate zone arithmetic. Each
// blob currently parses per spec/blob-format.json:
//
//   offset 0..1024   FP16 encoder input weight
//   offset 1024..    per-tensor FP32 biases interleaved with FP8 weights;
//                   each tensor boundary is aligned to 128 bytes
//   offset 130,976   zero padding to 128 KiB
//   total           131,072 B = 128 KiB exactly
//
// Five presets (quality/balanced/performance/ultraperf/native) share one
// identical blob; DRS is a separate retrained blob. The network architecture
// is identical across presets (RE README §"Weight Blob Analysis").
//
// The uint8 weights are codebook INDICES, not raw FP8 values. The FSR4
// shaders dequantize them via integer MAC that produces valid IEEE-754
// float32 bit patterns according to the RE notes. This remains a hypothesis
// until the local Vulkan proof produces numerically sane output.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace temporal_forge {

enum class Fsr4Preset : uint8_t {
    Quality,
    Balanced,
    Performance,
    UltraPerf,
    Native,
    Drs,            // dynamic resolution scaling — separate retrained blob
};

constexpr size_t kFsr4BlobSize = 131072;
// These legacy zone constants remain for loader ABI compatibility. The
// dispatch harness uses the physical per-tensor map recovered from the v4.1
// blob, not a single bias/weight split.
constexpr size_t kFsr4BiasZoneOffset = 0;
constexpr size_t kFsr4BiasZoneSize = 1024;
constexpr size_t kFsr4WeightZoneOffset = 1152;
constexpr size_t kFsr4WeightZoneSize = 129824;
constexpr size_t kFsr4ScaleZoneOffset = 130088;
constexpr size_t kFsr4ScaleZoneSize = 888;
constexpr size_t kFsr4PadZoneOffset = 130976;
constexpr size_t kFsr4PadZoneSize = 96;

struct Fsr4BlobView {
    // Zero-copy views into the loaded blob.
    const uint8_t* bytes = nullptr;        // full 131072 B
    const uint16_t* biasesFp16 = nullptr;  // legacy view: leading FP16 weight
    const uint8_t* weightsUint8 = nullptr; // legacy view: interleaved tensor data
    // Historical name retained for API stability. RE docs disagree on this
    // zone: blob-format calls it likely FP16 scale data, while
    // docs/extra-params-analysis.md classifies it as postpass FP32 params.
    const uint16_t* scalesFp16 = nullptr;
    const uint8_t* padding = nullptr;      // 96 B, must be zero
};

struct Fsr4BlobMeta {
    Fsr4Preset preset = Fsr4Preset::Quality;
    std::string name;
    bool isDrs = false;             // true if the separate DRS-retrained blob
    bool isStandardShared = false;  // true if one of the 5 identical presets
    uint32_t uniqueUint8Values = 0; // v4.1.0 should report 255
};

// Result of loading + validating one blob.
struct Fsr4BlobLoadResult {
    bool ok = false;
    Fsr4BlobMeta meta;
    std::string failReason;         // empty when ok
    std::vector<uint8_t> data;      // the 131072 bytes, owned
};

class WeightBlobLoader {
public:
    // Loads + validates a single blob from disk. Performs:
    //   - size check (exactly 131072 B)
    //   - padding check (last 96 B all zero)
    //   - zone-offset arithmetic
    //   - unique-uint8-value census (v4.1.0 expects 255)
    static Fsr4BlobLoadResult load(Fsr4Preset preset, const std::string& path);

    // Build a zero-copy view over a successfully-loaded blob's data.
    static Fsr4BlobView view(const Fsr4BlobLoadResult& loaded);

    // Preset metadata.
    static const char* presetName(Fsr4Preset p);
    static const char* presetFileName(Fsr4Preset p); // e.g. "quality.bin"
    static bool presetIsDrs(Fsr4Preset p);

    // Quick FNV-1a 64-bit hash for local identity checks (not cryptographic).
    static uint64_t fnv1a64(const uint8_t* data, size_t n);
};

} // namespace temporal_forge
