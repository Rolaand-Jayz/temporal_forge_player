// Fsr4ProofRunner.hpp — proof-gate validation for the FSR4 INT8 backend.
//
// Per the v3→10 INT8 policy doc: the experimental INT8 path runs only after
// proof passes. If proof fails, the system MUST fail closed to FSR 3.1.5.
//
// Proof states (policy doc):
//   FSR4_INT8_PROOF_REQUIRED  — not yet run
//   FSR4_INT8_PROOF_RUNNING   — in progress
//   FSR4_INT8_PROOF_PASSED    — structural validation succeeded; backend may be used
//   FSR4_INT8_PROOF_FAILED    — validation failed; fall back to FSR 3.1.5
//
// What the proof runner validates (honest scope):
//   Stage 1 — dispatch execution: the conv-chain pipeline runs without
//             Vulkan errors (device-lost, out-of-memory, shader crash).
//   Stage 2 — output sanity: the output buffers contain non-zero, finite
//             (non-NaN, non-Inf) values in a plausible range. This proves
//             the shaders actually computed something, not no-op'd.
//   Validation deliberately does not compare against AMD's Windows runtime
//   or a nonexistent official video-FSR reference. Local execution, finite
//   output, bounds, and visual A/B inspection are the project boundary.
//
// Diagnostic env knobs such as TFORGE_FSR4_FP8_SCALE may produce bounded
// tensors for investigation, but they cannot pass the proof gate.
//
// The proof runner is the reason the backend is labeled "experimental":
// it gates production use behind an honest validation boundary.
#pragma once
#include "backend/GpuCapabilityProbe.hpp"
#include "backend/UpscaleTypes.hpp"
#include "backend/WeightBlob.hpp"

#include <vulkan/vulkan.h>
#include <string>

namespace temporal_forge {

class Fsr4DispatchHarness;
struct Fsr4DispatchResources;

enum class Fsr4ProofState : uint8_t {
    Required,   // not yet run
    Running,    // in progress
    Passed,     // structural validation succeeded
    Failed,     // validation failed — fall back
};

struct Fsr4ProofResult {
    Fsr4ProofState state = Fsr4ProofState::Required;
    bool stage1DispatchOk = false;   // pipeline executed without Vulkan errors
    bool stage2OutputSane = false;    // output is non-zero, finite, bounded
    // Output statistics (for the stage-2 sanity check).
    uint64_t totalSamples = 0;
    uint64_t nonZeroSamples = 0;
    uint64_t nanInfSamples = 0;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    double meanValue = 0.0;
    double variance = 0.0;
    double dispatchMs = 0.0;
    std::string report;    // human-readable proof report
    std::string failReason;
};

class Fsr4ProofRunner {
public:
    // Run the proof validation on the given (already-initialized) harness.
    // The harness must have weights uploaded + resources allocated.
    // After this, read result.state to determine if the backend may be used.
    static Fsr4ProofResult run(Fsr4DispatchHarness& harness,
                               VkDevice device,
                               const Fsr4DispatchResources& res);

    [[nodiscard]] static const char* stateName(Fsr4ProofState s);
};

} // namespace temporal_forge
