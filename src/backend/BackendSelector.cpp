// BackendSelector.cpp — implements the fail-closed backend cascade documented
// in BackendSelector.hpp.
//
// Upstream: PlaybackEngine asks for a backend after Vulkan capability probing
// and whenever source/output dimensions change. Downstream: exactly one
// ITemporalUpscalerBackend receives frame packets. Keeping the cascade here
// prevents QML and playback code from knowing which experimental or fallback
// implementation is active.
#include "backend/BackendSelector.hpp"
#include "backend/Fsr4Int8Backend.hpp"
#include "backend/Fsr3FallbackBackend.hpp"
#include "backend/SpatialFallbackBackend.hpp"
#include "util/Log.hpp"

namespace temporal_forge {

// Constructor: allocate all candidate adapters once so fallback does not have
// to change object ownership or UI wiring while playback is running.
BackendSelector::BackendSelector() {
    fsr4_ = std::make_unique<Fsr4Int8Backend>();
    fsr3_ = std::make_unique<Fsr3FallbackBackend>();
    spatial_ = std::make_unique<SpatialFallbackBackend>();
}

// Destructor: unique_ptr members release each candidate backend. Individual
// backends own their resources and make destruction idempotent.
BackendSelector::~BackendSelector() = default;

// select: try the preferred experimental backend, then the real SDK fallback,
// then the always-available spatial path. The returned pointer is owned by this
// selector and remains stable until the next recreation/fallback operation.
ITemporalUpscalerBackend* BackendSelector::select(const UpscaleContextDesc& desc) {
    fellBack_ = false;
    proofAttempted_ = false;
    proofPassed_ = false;

    // 1. FSR4 INT8 (experimental, proof-gated) — default on RDNA3.
    if (fsr4_ && fsr4_->ready()) {
        if (fsr4_->create(desc)) {
            // Proof gate: run the validation before trusting the backend.
            // (The Fsr4ProofRunner is invoked by the caller after create();
            //  here we mark the backend as created-pending-proof.)
            active_ = fsr4_.get();
            activeKind_ = BackendKind::Fsr4ReExperimental;
            proofAttempted_ = false; // proof runs on first dispatch attempt
            return active_;
        }
        logWarn("BackendSelector: FSR4 INT8 create failed; cascading to fallback.");
        fellBack_ = true;
    }

    // 2. FSR 3.1.5 fallback (real temporal upscaler).
    if (fsr3_->sdkAvailable() && fsr3_->create(desc)) {
        active_ = fsr3_.get();
        activeKind_ = BackendKind::Fsr23Sdk;
        return active_;
    }
    if (fsr3_->sdkAvailable()) {
        logWarn("BackendSelector: FSR 3.1.5 create failed; cascading to spatial.");
    } else {
        logInfo("BackendSelector: FSR 3.1.5 SDK not linked; using spatial fallback.");
    }
    fellBack_ = true;

    // 3. Spatial fallback (always available).
    spatial_->create(desc);
    active_ = spatial_.get();
    activeKind_ = BackendKind::SpatialFallback;
    return active_;
}

// fallbackToNext: move one step down the policy after a proof or dispatch
// failure. The spatial backend is the floor, so callers always receive a
// usable adapter even when no temporal implementation is available.
ITemporalUpscalerBackend* BackendSelector::fallbackToNext(const UpscaleContextDesc& desc) {
    if (activeKind_ == BackendKind::Fsr4ReExperimental) {
        logWarn("BackendSelector: FSR4 INT8 proof failed; falling to FSR 3.1.5.");
        if (fsr3_->sdkAvailable() && fsr3_->create(desc)) {
            active_ = fsr3_.get();
            activeKind_ = BackendKind::Fsr23Sdk;
            fellBack_ = true;
            return active_;
        }
    }
    if (activeKind_ == BackendKind::Fsr23Sdk) {
        logWarn("BackendSelector: FSR 3.1.5 failed; falling to spatial.");
    }
    spatial_->create(desc);
    active_ = spatial_.get();
    activeKind_ = BackendKind::SpatialFallback;
    fellBack_ = true;
    return active_;
}

// activeLabel: convert internal backend identity plus fallback state into the
// wording shown by QML. This is deliberately centralized so an experimental
// path cannot accidentally be presented as an official implementation.
const char* BackendSelector::activeLabel() const {
    // Policy wording (v3→10 doc): never "official" for the INT8 path.
    switch (activeKind_) {
        case BackendKind::Fsr4ReExperimental:
            return fellBack_ ? "FSR 4.1 INT8 (experimental — proof failed, fallback active)"
                             : "FSR 4.1 INT8 — RDNA3, RE-derived, experimental";
        case BackendKind::Fsr23Sdk:
            return "FSR 3.1.5 (fallback)";
        case BackendKind::SpatialFallback:
            return "Spatial fallback";
        case BackendKind::Null:
            return "Null";
    }
    return "Unknown";
}

// recreateForChange: tear down dimension-dependent state in every candidate,
// then rerun selection for the new context. Window-only presentation changes
// must not call this; PlaybackEngine filters those out before reaching here.
bool BackendSelector::recreateForChange(const UpscaleContextDesc& desc) {
    fellBack_ = false;
    fsr4_->destroy();
    fsr3_->destroy();
    spatial_->destroy();
    select(desc);
    return active_ != nullptr;
}

} // namespace temporal_forge
