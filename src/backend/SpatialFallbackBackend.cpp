// SpatialFallbackBackend.cpp
#include "backend/SpatialFallbackBackend.hpp"

namespace temporal_forge {

SpatialFallbackBackend::SpatialFallbackBackend(SpatialMode mode) : mode_(mode) {}

void SpatialFallbackBackend::setMode(SpatialMode m) { mode_ = m; }

bool SpatialFallbackBackend::create(const UpscaleContextDesc& desc) {
    info_ = {};
    info_.name = "Spatial fallback";
    info_.version = "1.0";
    info_.backendKind = BackendKind::SpatialFallback;
    info_.activePreset = desc.preset;
    info_.sourceWidth = desc.sourceWidth;
    info_.sourceHeight = desc.sourceHeight;
    info_.fsrOutputWidth = desc.fsrOutputWidth;
    info_.fsrOutputHeight = desc.fsrOutputHeight;
    info_.historyValid = true;
    info_.lastError = UpscaleError::None;
    created_ = true;
    return true;
}

bool SpatialFallbackBackend::reconfigure(const UpscaleContextDesc& desc) {
    if (!created_) return create(desc);
    info_.activePreset = desc.preset;
    info_.sourceWidth = desc.sourceWidth;
    info_.sourceHeight = desc.sourceHeight;
    info_.fsrOutputWidth = desc.fsrOutputWidth;
    info_.fsrOutputHeight = desc.fsrOutputHeight;
    return true;
}

UpscaleOutputPacket SpatialFallbackBackend::dispatch(const VideoFsrPacket& packet) {
    // Spatial pass-through: the presentation scaler (PresentScaler / image
    // provider) performs the actual spatial resample. Here we just record
    // the target dimensions and mark the output as the input color at the
    // requested output size.
    UpscaleOutputPacket out;
    out.output = packet.color;
    out.width = packet.fsrOutputWidth;
    out.height = packet.fsrOutputHeight;
    // Spatial path is always "history valid" — no temporal state to reset.
    out.historyValid = true;
    info_.lastDispatchMs = 0.0;
    // Increment reset count only to reflect that we observed a reset request,
    // but it has no effect on this stateless backend.
    if (packet.reset) info_.resetCount++;
    return out;
}

void SpatialFallbackBackend::resetHistory() {
    // No-op: spec 04 section 3 — spatial requires no temporal resources.
}

void SpatialFallbackBackend::destroy() {
    created_ = false;
    info_ = {};
}

} // namespace temporal_forge
