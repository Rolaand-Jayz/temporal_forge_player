// NullBackend.cpp
#include "backend/NullBackend.hpp"

namespace temporal_forge {

bool NullBackend::create(const UpscaleContextDesc& desc) {
    info_ = {};
    info_.name = "Null";
    info_.version = "1.0";
    info_.backendKind = BackendKind::Null;
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

bool NullBackend::reconfigure(const UpscaleContextDesc& desc) {
    if (!created_) return create(desc);
    info_.activePreset = desc.preset;
    info_.sourceWidth = desc.sourceWidth;
    info_.sourceHeight = desc.sourceHeight;
    info_.fsrOutputWidth = desc.fsrOutputWidth;
    info_.fsrOutputHeight = desc.fsrOutputHeight;
    return true;
}

UpscaleOutputPacket NullBackend::dispatch(const VideoFsrPacket& packet) {
    // Identity: pass the input color straight through as the output.
    UpscaleOutputPacket out;
    out.output = packet.color;
    out.width = packet.fsrOutputWidth;
    out.height = packet.fsrOutputHeight;
    out.historyValid = !packet.reset;
    info_.lastDispatchMs = 0.0;
    if (packet.reset) {
        info_.resetCount++;
        info_.historyValid = false;
    } else {
        info_.historyValid = true;
    }
    return out;
}

void NullBackend::destroy() {
    created_ = false;
    info_ = {};
}

} // namespace temporal_forge
