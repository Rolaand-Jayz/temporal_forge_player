// Fsr23SdkBackend.hpp — spec 04 section 1: FSR 2.3 SDK stable backend.
//
// Integrates AMD's FSR2 host API:
//   ffxFsr2ContextCreate / ffxFsr2ContextDispatch / ffxFsr2ContextDestroy
//
// Mapping (spec 04 section 1):
//   VideoFsrPacket.color    -> FSR color input
//   VideoFsrPacket.depth    -> FSR depth input
//   VideoFsrPacket.motion   -> FSR motion vector input
//   VideoFsrPacket.reactive -> FSR reactive mask input
//   VideoFsrPacket.tcMask    -> FSR transparency/composition input
//   VideoFsrPacket.exposure -> FSR exposure input (or null w/ auto exposure)
//   jitterX/Y               -> jitter offset
//   frameTimeMs             -> frameTimeDelta
//   reset                   -> reset flag
//
// When compiled without the FSR SDK available (TFORGE_HAVE_FSR2_SDK unset),
// create() returns false with UnsupportedDevice and the BackendSelector
// falls back to SpatialFallbackBackend. This keeps the architecture correct
// and the FSR2 path ready to activate once the SDK builds on this platform.
//
// FAKE CAMERA VALUES (spec 03/04): video has no real camera.
//   cameraNear = 0.1, cameraFar = 1000.0,
//   cameraFovAngleVertical = 1.0472 (60deg), viewSpaceToMetersFactor = 1.0
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"

namespace temporal_forge {

// Set at CMake configure time when the FSR2 SDK is linked.
#ifndef TFORGE_HAVE_FSR2_SDK
#define TFORGE_HAVE_FSR2_SDK 0
#endif

class Fsr23SdkBackend : public ITemporalUpscalerBackend {
public:
    Fsr23SdkBackend();
    ~Fsr23SdkBackend() override;

    [[nodiscard]] BackendInfo info() const override { return info_; }
    bool create(const UpscaleContextDesc& desc) override;
    bool reconfigure(const UpscaleContextDesc& desc) override;
    UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) override;
    void resetHistory() override;
    void destroy() override;

    [[nodiscard]] bool sdkAvailable() const;

private:
    BackendInfo info_;
    bool created_ = false;
#if TFORGE_HAVE_FSR2_SDK
    struct Fsr2State;
    Fsr2State* state_ = nullptr; // opaque FfxFsr2Context + scratch
#endif
};

} // namespace temporal_forge
