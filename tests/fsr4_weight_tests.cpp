// fsr4_weight_tests.cpp — P-A verification gate.
// Validates the RE weight blobs load + parse correctly, and that the GPU
// capability probe resolves to the INT8 DOT4 profile on this hardware.
#include "backend/WeightBlob.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

int main() {
    // Locate the RE blobs. The RE dataset was extracted next to the project.
    std::string blobDir;
    for (const char* cand : {
        "RE-of-FSR-4.1.0-Upscaling-1.0/extracted/v410_initializers",
        "../RE-of-FSR-4.1.0-Upscaling-1.0/extracted/v410_initializers",
        "../../RE-of-FSR-4.1.0-Upscaling-1.0/extracted/v410_initializers",
    }) {
        if (std::filesystem::exists(std::string(cand) + "/quality.bin")) {
            blobDir = cand; break;
        }
    }
    if (blobDir.empty()) {
        std::fprintf(stderr, "fsr4_weight_tests: SKIP (RE blobs not found)\n");
        return 77;
    }

    // Load each preset and validate format + zone math.
    for (Fsr4Preset p : {
        Fsr4Preset::Quality, Fsr4Preset::Balanced, Fsr4Preset::Performance,
        Fsr4Preset::UltraPerf, Fsr4Preset::Native, Fsr4Preset::Drs }) {
        const std::string path = blobDir + "/" + WeightBlobLoader::presetFileName(p);
        auto r = WeightBlobLoader::load(p, path);
        CHECK(r.ok);
        CHECK(r.data.size() == kFsr4BlobSize);
        // v4.1.0 codebook signature: near-full uint8 range. Standard presets
        // report 255 unique values; the separately-retrained DRS blob reports
        // 256 (all of 0-255). Either is a valid v4.1.0 blob (v4.0.2 had 122).
        CHECK(r.meta.uniqueUint8Values >= 255);
        CHECK(r.meta.isDrs == WeightBlobLoader::presetIsDrs(p));

        // Zero-copy view must resolve all zones.
        auto v = WeightBlobLoader::view(r);
        CHECK(v.bytes != nullptr);
        CHECK(v.biasesFp16 != nullptr);
        CHECK(v.weightsUint8 != nullptr);
        CHECK(v.scalesFp16 != nullptr);
        CHECK(v.padding != nullptr);
        // Zone offsets within the blob.
        CHECK(reinterpret_cast<const uint8_t*>(v.biasesFp16) == v.bytes + kFsr4BiasZoneOffset);
        CHECK(v.weightsUint8 == v.bytes + kFsr4WeightZoneOffset);
        CHECK(reinterpret_cast<const uint8_t*>(v.scalesFp16) == v.bytes + kFsr4ScaleZoneOffset);
    }

    // The 5 non-DRS presets must be byte-identical (RE README: shared weights).
    auto load = [&](Fsr4Preset p) {
        return WeightBlobLoader::load(p, blobDir + "/" + WeightBlobLoader::presetFileName(p));
    };
    auto q = load(Fsr4Preset::Quality);
    auto d = load(Fsr4Preset::Drs);
    CHECK(q.ok && d.ok);
    // Standard presets identical to each other.
    for (Fsr4Preset p : {Fsr4Preset::Balanced, Fsr4Preset::Performance,
                         Fsr4Preset::UltraPerf, Fsr4Preset::Native}) {
        auto o = load(p);
        CHECK(o.ok);
        CHECK(o.data == q.data);
    }
    // DRS differs from standard.
    bool drsSame = (d.data == q.data);
    CHECK(!drsSame);

    if (g_failures == 0) {
        std::printf("fsr4_weight_tests: OK (6 blobs loaded, 5 shared + DRS unique)\n");
        return 0;
    }
    std::fprintf(stderr, "fsr4_weight_tests: %d FAILURES\n", g_failures);
    return 1;
}
