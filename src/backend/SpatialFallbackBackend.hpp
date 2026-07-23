// SpatialFallbackBackend.hpp — spec 04 section 3.
// Always-available reliability path. No temporal history, no motion vectors,
// no fake depth, no reactive mask required.
//
// Use cases (spec 04): unsupported GPU, FSR backend failure, debug
// comparisons, low-power mode, late-frame fallback.
//
// This backend is the GUARANTEED working path: if any temporal backend fails,
// the BackendSelector falls back here and playback continues (spec 01
// "Failure Policy", spec 08 risk #7 mitigation).
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"

namespace temporal_forge {

enum class SpatialMode {
    Bilinear,
    Bicubic,
    Lanczos,
    Easu, // FSR EASU+RCAS if available
};

class SpatialFallbackBackend : public ITemporalUpscalerBackend {
public:
    explicit SpatialFallbackBackend(SpatialMode mode = SpatialMode::Bicubic);

    [[nodiscard]] BackendInfo info() const override { return info_; }
    bool create(const UpscaleContextDesc& desc) override;
    bool reconfigure(const UpscaleContextDesc& desc) override;
    UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) override;
    void resetHistory() override; // no-op: spatial has no history (spec 04)
    void destroy() override;

    void setMode(SpatialMode m);

private:
    SpatialMode mode_;
    BackendInfo info_;
    bool created_ = false;
};

} // namespace temporal_forge
