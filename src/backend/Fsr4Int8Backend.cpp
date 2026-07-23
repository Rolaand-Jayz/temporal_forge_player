// Fsr4Int8Backend.cpp
#include "backend/Fsr4Int8Backend.hpp"
#include "render/Fsr4DispatchHarness.hpp"
#include "util/Log.hpp"

namespace temporal_forge {

Fsr4Int8Backend::Fsr4Int8Backend() {
    info_.name = "FSR 4.1 INT8 (RDNA3, RE-derived, experimental)";
    info_.version = "4.1.1-INT8";
    info_.backendKind = BackendKind::Fsr4ReExperimental;
}

Fsr4Int8Backend::~Fsr4Int8Backend() { destroy(); }

bool Fsr4Int8Backend::ready() const { return harness_ && harness_->initialized(); }

void Fsr4Int8Backend::setDevice(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                 uint32_t queueFamily, const GpuCapability& cap) {
    physical_ = physical; device_ = device; queue_ = queue;
    queueFamily_ = queueFamily; cap_ = cap;
}

void Fsr4Int8Backend::setWeightBlob(const Fsr4BlobView& b) { blob_ = b; }

bool Fsr4Int8Backend::create(const UpscaleContextDesc& desc) {
    info_.activePreset = desc.preset;
    info_.sourceWidth = desc.sourceWidth;
    info_.sourceHeight = desc.sourceHeight;
    info_.fsrOutputWidth = desc.fsrOutputWidth;
    info_.fsrOutputHeight = desc.fsrOutputHeight;

    if (!cap_.valid) {
        info_.lastError = UpscaleError::UnsupportedDevice;
        logWarn("Fsr4Int8Backend: GPU capability not viable ({})", cap_.failReason);
        return false;
    }
    if (device_ == VK_NULL_HANDLE) {
        info_.lastError = UpscaleError::ContextCreateFailed;
        logWarn("Fsr4Int8Backend: no Vulkan device set");
        return false;
    }

    harness_ = std::make_unique<Fsr4DispatchHarness>();
    if (!harness_->init(physical_, device_, queue_, queueFamily_, cap_)) {
        info_.lastError = UpscaleError::ContextCreateFailed;
        return false;
    }

    Fsr4DispatchResources r{};
    r.sourceWidth = desc.sourceWidth;
    r.sourceHeight = desc.sourceHeight;
    r.outputWidth = desc.fsrOutputWidth;
    r.outputHeight = desc.fsrOutputHeight;
    if (!harness_->allocateResources(r)) {
        info_.lastError = UpscaleError::OutOfMemory;
        return false;
    }
    if (!harness_->uploadWeights(blob_)) {
        info_.lastError = UpscaleError::WeightValidationFailed;
        return false;
    }

    info_.lastError = UpscaleError::None;
    logInfo("Fsr4Int8Backend: created (EXPERIMENTAL) {}x{} -> {}x{} on {}",
            desc.sourceWidth, desc.sourceHeight,
            desc.fsrOutputWidth, desc.fsrOutputHeight, cap_.deviceName);
    return true;
}

bool Fsr4Int8Backend::reconfigure(const UpscaleContextDesc& desc) {
    destroy();
    return create(desc);
}

UpscaleOutputPacket Fsr4Int8Backend::dispatch(const VideoFsrPacket& packet) {
    UpscaleOutputPacket out;
    if (!harness_) { info_.lastError = UpscaleError::DispatchFailed; return out; }
    auto r = harness_->dispatchFrame(packet.reset);
    if (!r.ok) {
        info_.lastError = r.error;
        return out;
    }
    if (packet.reset) info_.resetCount++;
    info_.historyValid = !packet.reset;
    info_.lastDispatchMs = r.dispatchMs;
    out.width = packet.fsrOutputWidth;
    out.height = packet.fsrOutputHeight;
    out.historyValid = info_.historyValid;
    return out;
}

void Fsr4Int8Backend::resetHistory() {
    if (harness_) harness_->dispatchFrame(/*reset=*/true);
    info_.resetCount++;
}

void Fsr4Int8Backend::destroy() {
    harness_.reset();
}

} // namespace temporal_forge
