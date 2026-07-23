// Fsr4ProofRunner.cpp
#include "backend/Fsr4ProofRunner.hpp"
#include "render/Fsr4DispatchHarness.hpp"
#include "util/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace temporal_forge {

const char* Fsr4ProofRunner::stateName(Fsr4ProofState s) {
    switch (s) {
        case Fsr4ProofState::Required: return "FSR4_INT8_PROOF_REQUIRED";
        case Fsr4ProofState::Running:  return "FSR4_INT8_PROOF_RUNNING";
        case Fsr4ProofState::Passed:   return "FSR4_INT8_PROOF_PASSED";
        case Fsr4ProofState::Failed:   return "FSR4_INT8_PROOF_FAILED";
    }
    return "?";
}

Fsr4ProofResult Fsr4ProofRunner::run(Fsr4DispatchHarness& harness,
                                     VkDevice device,
                                     const Fsr4DispatchResources& res) {
    Fsr4ProofResult result;
    result.state = Fsr4ProofState::Running;

    std::ostringstream rep;
    rep << "=== FSR4 INT8 Proof Report ===\n";
    rep << "Device: " << harness.capability().deviceName << "\n";
    rep << "Profile: " << GpuCapabilityProbe::profileName(harness.capability().profile) << "\n";
    rep << "Source: " << res.sourceWidth << "x" << res.sourceHeight << "\n";
    rep << "Output: " << res.outputWidth << "x" << res.outputHeight << "\n\n";
    const char* diagnosticFp8Scale = std::getenv("TFORGE_FSR4_FP8_SCALE");
    const bool diagnosticScaleActive = diagnosticFp8Scale && diagnosticFp8Scale[0] != '\0';
    if (diagnosticScaleActive) {
        rep << "Diagnostic FP8 scale: " << diagnosticFp8Scale << "\n";
        rep << "NOTE: diagnostic scaling cannot pass the proof gate or enable the backend.\n\n";
    }

    // --- Stage 1: dispatch execution ---
    rep << "Stage 1: dispatch execution\n";
    auto dispatchResult = harness.dispatchFrame(/*reset=*/true);
    if (!dispatchResult.ok) {
        rep << "  FAILED: " << dispatchResult.failReason
            << " (error " << static_cast<int>(dispatchResult.error) << ")\n";
        result.state = Fsr4ProofState::Failed;
        result.failReason = "Stage 1 (dispatch): " + dispatchResult.failReason;
        result.report = rep.str();
        logWarn("Fsr4ProofRunner: Stage 1 FAILED — {}", result.failReason);
        return result;
    }
    result.stage1DispatchOk = true;
    result.dispatchMs = dispatchResult.dispatchMs;
    rep << "  PASSED: conv-chain dispatch completed in " << dispatchResult.dispatchMs << " ms\n\n";

    // --- Stage 2: output sanity ---
    // Read back the final accum slot, not the whole scratch buffer. Scratch is
    // seeded with synthetic input for the proof, so scanning all of it can pass
    // even when the conv chain collapses or stops propagating spatial signal.
    rep << "Stage 2: output sanity\n";
    if (res.accumBuffer == VK_NULL_HANDLE || res.accumMemory == VK_NULL_HANDLE) {
        rep << "  SKIPPED: no accum buffer to read back\n";
        result.stage2OutputSane = false;
        result.state = Fsr4ProofState::Failed;
        result.failReason = "Stage 2: no readable output buffer";
        result.report = rep.str();
        return result;
    }

    // Working tensors are device-local. Read the exact final tensor through a
    // temporary staging buffer instead of assuming VRAM is mappable.
    std::vector<float> readback;
    if (!harness.readbackFinalAccum(readback)) {
        rep << "  FAILED: cannot map accum buffer for readback\n";
        result.state = Fsr4ProofState::Failed;
        result.failReason = "Stage 2: accum buffer map failed";
        result.report = rep.str();
        return result;
    }
    const VkDeviceSize finalOffset = harness.finalAccumOffsetBytes();
    const float* data = readback.data();
    const size_t count = readback.size();
    if (const char* dumpPath = std::getenv("TFORGE_FSR4_DUMP_FINAL")) {
        std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
        if (dump) {
            dump.write(reinterpret_cast<const char*>(data),
                       static_cast<std::streamsize>(count * sizeof(float)));
        }
    }
    // Sample a subset for speed (every Nth element if the buffer is huge).
    const size_t sampleStride = (count > 1000000) ? count / 1000000 : 1;
    uint64_t nonZero = 0, nanInf = 0;
    double sum = 0.0, sumSq = 0.0;
    float mn = 1e30f, mx = -1e30f;
    float maxAbs = 0.0f;
    uint64_t sampled = 0;
    for (size_t i = 0; i < count; i += sampleStride) {
        float v = data[i];
        if (std::isnan(v) || std::isinf(v)) { ++nanInf; continue; }
        if (v != 0.0f) ++nonZero;
        maxAbs = std::max(maxAbs, std::abs(v));
        sum += v;
        sumSq += double(v) * double(v);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        ++sampled;
    }
    result.totalSamples = sampled;
    result.nonZeroSamples = nonZero;
    result.nanInfSamples = nanInf;
    result.minValue = (sampled > 0) ? mn : 0.0f;
    result.maxValue = (sampled > 0) ? mx : 0.0f;
    result.meanValue = (sampled > 0) ? sum / sampled : 0.0;
    result.variance = (sampled > 0) ? (sumSq / sampled - result.meanValue * result.meanValue) : 0.0;

    rep << "  Final accum offset: " << finalOffset << " bytes\n";
    rep << "  Samples: " << sampled << " (stride " << sampleStride << " of " << count << ")\n";
    rep << "  Non-zero: " << nonZero << " (" << (sampled ? 100.0*nonZero/sampled : 0.0) << "%)\n";
    rep << "  NaN/Inf: " << nanInf << "\n";
    rep << "  Range: [" << result.minValue << ", " << result.maxValue << "]\n";
    rep << "  Max abs: " << maxAbs << "\n";
    rep << "  Mean: " << result.meanValue << "  Variance: " << result.variance << "\n";

    // Stage-2 pass criteria: the conv passes ran on the weights and produced
    // *something*. All-zero or all-NaN output means the pipeline no-op'd or
    // crashed. Non-zero + finite + non-trivial variance = structurally sane.
    const bool hasNonZero = nonZero > (sampled / 100); // >1% non-zero
    const bool noCorruption = nanInf == 0;
    const bool hasVariance = result.variance > 1e-12;
    const bool bounded = maxAbs < 1.0e6f;
    result.stage2OutputSane = hasNonZero && noCorruption && hasVariance && bounded;

    if (result.stage2OutputSane) {
        rep << "  PASSED: output is non-zero, finite, with non-trivial variance\n\n";
    } else {
        rep << "  FAILED: output failed sanity check"
            << (hasNonZero ? "" : " [all-zero]")
            << (noCorruption ? "" : " [NaN/Inf present]")
            << (hasVariance ? "" : " [no variance]")
            << (bounded ? "" : " [runaway magnitude]") << "\n\n";
    }

    // Final verdict.
    rep << "=== Verdict ===\n";
    if (result.stage1DispatchOk && result.stage2OutputSane && !diagnosticScaleActive) {
        result.state = Fsr4ProofState::Passed;
        rep << "PROOF PASSED (structural): the FSR4 INT8 pipeline executes\n";
        rep << "  correctly and produces structurally-sane output. The backend\n";
        rep << "  may be used in EXPERIMENTAL video mode. Local structural\n";
        rep << "  sanity is the validation boundary for this project.\n";
        logInfo("Fsr4ProofRunner: PASSED (structural, {} ms, {} non-zero/{} samples)",
                result.dispatchMs, result.nonZeroSamples, result.totalSamples);
    } else {
        result.state = Fsr4ProofState::Failed;
        rep << "PROOF FAILED: ";
        if (!result.stage1DispatchOk) rep << "dispatch did not complete. ";
        if (!result.stage2OutputSane) rep << "output failed sanity. ";
        if (diagnosticScaleActive) rep << "diagnostic FP8 scale is active. ";
        rep << "\nFalling back to FSR 3.1.5 per the fail-closed policy.\n";
        logWarn("Fsr4ProofRunner: FAILED — state={} stage2Sane={} diagnosticScale={}",
                stateName(result.state), result.stage2OutputSane,
                diagnosticScaleActive ? "yes" : "no");
    }

    result.report = rep.str();
    return result;
}

} // namespace temporal_forge
