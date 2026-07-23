// BackendSelector.hpp — FSR 4.1 INT8 on RDNA3 backend selection policy.
//
// Policy (v3→10 INT8 doc + the corrected FSR 4.1.1 RDNA3 understanding):
//
//   1. FSR4_INT8_EXPERIMENTAL_DEFAULT  — on detected RX 7000 / RDNA3.
//        RE-derived, experimental label, PROOF-GATED. If the proof runner
//        fails, fall closed. This is the default path on your hardware.
//   2. FSR 3.1.5 fallback              — real temporal upscaler from open
//        SDK source. The fail-closed path beneath INT8.
//   3. Spatial fallback                — always-available reliability path.
//   4. Null / CPU                      — bring-up / test.
//
// The selector runs the FSR4 INT8 proof gate on first use. If proof fails,
// it cascades to FSR 3.1.5; if that's unavailable (SDK not linked), to
// spatial. The GUI never presents INT8 as "official" — correct wording only.
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"
#include "backend/UpscaleTypes.hpp"
#include <memory>
#include <string>

namespace temporal_forge {

class Fsr4Int8Backend;     // forward (lives in backend/, added in P-G wiring)
class Fsr3FallbackBackend;
class SpatialFallbackBackend;
class GpuCapabilityProbe;
struct GpuCapability;
class Fsr4ProofRunner;
struct Fsr4ProofResult;

class BackendSelector {
public:
    BackendSelector();
    ~BackendSelector();

    // select: resolve the active backend per the cascade policy.
    //
    // Called by: PlaybackEngine::initFsr4Path (once, when the FSR4 path is set up).
    // Calls:     GpuCapabilityProbe::probe, Fsr4Int8Backend::create, Fsr4ProofRunner::run;
    //            on proof failure or INT8 unavailability, falls through to fallbackToNext.
    // Returns:   the active backend (never null). Auto-picks based on GPU capability
    //            and runs the FSR4 INT8 proof gate if RDNA3 is detected.
    ITemporalUpscalerBackend* select(const UpscaleContextDesc& desc);

    // fallbackToNext: cascade down the policy — INT8 -> FSR3.1.5 -> spatial.
    //
    // Called by: select (on INT8 create/proof failure) and recreateForChange
    //            (on a backend failing to recreate).
    // Returns:   the next viable backend (spatial is the always-available floor).
    ITemporalUpscalerBackend* fallbackToNext(const UpscaleContextDesc& desc);

    // Trivial accessors: activeKind/active report the resolved backend + kind;
    //                    fellBack reports whether a cascade occurred;
    //                    activeLabel returns the user-facing wording (INT8 is
    //                    never labeled "official" — correct wording only).
    [[nodiscard]] BackendKind activeKind() const { return activeKind_; }
    [[nodiscard]] ITemporalUpscalerBackend* active() const { return active_; }
    [[nodiscard]] bool fellBack() const { return fellBack_; }
    [[nodiscard]] const char* activeLabel() const;  // user-facing per policy wording

    // recreateForChange: recreate the backend on source/preset/backend/resolution
    //                    change (NOT on window resize — that is presentation-only).
    //                    Called by: PlaybackEngine::setFsrViewport (preset ratio change).
    //                    Returns: true if the (possibly new) backend created cleanly.
    bool recreateForChange(const UpscaleContextDesc& desc);

private:
    std::unique_ptr<Fsr4Int8Backend> fsr4_;
    std::unique_ptr<Fsr3FallbackBackend> fsr3_;
    std::unique_ptr<SpatialFallbackBackend> spatial_;
    ITemporalUpscalerBackend* active_ = nullptr;
    BackendKind activeKind_ = BackendKind::SpatialFallback;
    bool fellBack_ = false;
    bool proofAttempted_ = false;
    bool proofPassed_ = false;
};

} // namespace temporal_forge
