// ITemporalUpscalerBackend: spec 04 backend interface. Every backend (FSR2.3
// SDK, FSR4-RE experimental, spatial fallback, null) receives the same
// high-level VideoFsrPacket. The player does not care which is active.
//
// Lifecycle: create() -> [reconfigure() on preset/source change] -> dispatch()
//            per frame -> resetHistory() on seek/scene-cut/new-file -> destroy().
// Called by: BackendSelector owns and selects backends; PlaybackEngine's decode
//            thread calls dispatch() per upscaled frame. Thread-safety: dispatch
//            is called from the decode thread only; create/reconfigure/destroy
//            from the UI thread under the dispatch mutex.
//
// Contract rules (spec 04 section 9):
//   - never change frame rate
//   - never generate frames
//   - never own subtitle/UI composition
//   - never recreate itself on window resize
//   - preserve packet timestamp identity
//   - support resetHistory()
//   - fail gracefully
#pragma once
#include "backend/UpscaleTypes.hpp"

namespace temporal_forge {

class ITemporalUpscalerBackend {
public:
    virtual ~ITemporalUpscalerBackend() = default;

    // The six lifecycle methods every backend implements (see lifecycle note above).
    [[nodiscard]] virtual BackendInfo info() const = 0;
    virtual bool create(const UpscaleContextDesc& desc) = 0;
    virtual bool reconfigure(const UpscaleContextDesc& desc) = 0;
    virtual UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) = 0;
    virtual void resetHistory() = 0;
    virtual void destroy() = 0;
};

} // namespace temporal_forge
