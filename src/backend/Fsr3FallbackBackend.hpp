// Fsr3FallbackBackend.hpp — FSR 3.1.5 fail-closed fallback backend.
//
// Per the v3→10 INT8 policy doc: FSR 3.1.5 is the REQUIRED fail-closed
// fallback beneath the experimental FSR4 INT8 path. When the FSR4 INT8
// proof fails (or the GPU doesn't support INT8 cooperative matrix), the
// selector falls to this backend — a real temporal upscaler, not spatial-
// only, so the player is never left with only spatial scaling.
//
// FSR 3.1.5 is open-source in the AMD FidelityFX SDK (the release-FSR3-3.0.4
// branch, MIT-licensed). Unlike FSR4 (closed binary, RE-derived), FSR3's
// source + HLSL shaders are fully open. The host API is:
//   ffxFsr3ContextCreate / ffxFsr3ContextDispatch / ffxFsr3ContextDestroy
//
// When the SDK builds as a CMake subproject (TFORGE_HAVE_FSR3_SDK=1), this
// backend links the FSR3 component + Vulkan backend and performs real
// temporal upscaling. When the SDK isn't linked, it reports UnsupportedDevice
// and the selector falls through to the spatial fallback — honest, never
// silently broken.
//
// The FSR4 strip-plan doc confirms: for video upscaling, FSR3 uses the same
// temporal input bundle (color, depth, motion, jitter, reset, frameTime).
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"

namespace temporal_forge {

#ifndef TFORGE_HAVE_FSR3_SDK
#define TFORGE_HAVE_FSR3_SDK 0
#endif

// Fsr3FallbackBackend: the FSR 3.1.5 fail-closed fallback backend — the middle
// tier of the cascade beneath the experimental FSR4 INT8 path. When TFORGE_HAVE_FSR3_SDK
// is 0 (the typical Linux build), create() fails honestly and the selector falls
// through to SpatialFallbackBackend — honest, never silently broken.
//
// Lifecycle follows ITemporalUpscalerBackend. When the SDK IS linked, create()
// allocates an FfxFsr3Context; dispatch() runs the real temporal upscale.
class Fsr3FallbackBackend : public ITemporalUpscalerBackend {
public:
    Fsr3FallbackBackend();
    ~Fsr3FallbackBackend() override;

    // info: per-backend telemetry (name "FSR 3.1.5 fallback", version, kind).
    [[nodiscard]] BackendInfo info() const override { return info_; }
    // create: allocate the FSR3 context. Fails honestly (returns false) when the
    //         SDK is not linked so the selector cascades to spatial. Called by
    //         BackendSelector::fallbackToNext.
    bool create(const UpscaleContextDesc& desc) override;
    // reconfigure: re-create the context on source/preset/resolution change.
    bool reconfigure(const UpscaleContextDesc& desc) override;
    // dispatch: run one frame through the FSR3 temporal upscale. Called by the
    //           decode thread via BackendSelector. Preserves packet timestamp identity.
    UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) override;
    // resetHistory: invalidate temporal history (seek/scene-cut/new-file).
    void resetHistory() override;
    // destroy: release the FSR3 context + resources.
    void destroy() override;

    [[nodiscard]] bool sdkAvailable() const;

private:
    BackendInfo info_;
    bool created_ = false;
#if TFORGE_HAVE_FSR3_SDK
    struct Fsr3State;
    Fsr3State* state_ = nullptr;
#endif
};

} // namespace temporal_forge
