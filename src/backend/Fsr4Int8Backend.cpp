// Fsr4Int8Backend.cpp — adapter between the backend-selection policy and the
// Vulkan FSR4 dispatch harness.
//
// Upstream: BackendSelector supplies the device, weight blob, and resolved
// source/output dimensions. Downstream: Fsr4DispatchHarness owns Vulkan
// resources and executes the recovered prepass/conv/postpass chain. This file
// deliberately contains no reconstruction math; it translates lifecycle and
// frame-packet contracts between those two components and reports failures so
// BackendSelector can fall back safely.
#include "backend/Fsr4Int8Backend.hpp"
#include "render/Fsr4DispatchHarness.hpp"
#include "util/Log.hpp"

namespace temporal_forge {

// Constructor: publish the honest user-facing identity of this backend before
// selection or proof-gating begins. The label stays experimental because the
// implementation is RE-derived rather than an official AMD runtime.
Fsr4Int8Backend::Fsr4Int8Backend() {
    info_.name = "FSR 4.1 INT8 (RDNA3, RE-derived, experimental)";
    info_.version = "4.1.1-INT8";
    info_.backendKind = BackendKind::Fsr4ReExperimental;
}

// Destructor: release the harness before the backend object leaves scope. The
// harness destructor owns the Vulkan resource teardown; callers normally reach
// here through BackendSelector shutdown.
Fsr4Int8Backend::~Fsr4Int8Backend() { destroy(); }

// ready: report whether the harness has completed device initialization. This
// is a local readiness check used by selectors and diagnostics, not a claim
// that a frame can be dispatched until create() has also allocated resources.
bool Fsr4Int8Backend::ready() const { return harness_ && harness_->initialized(); }

// setDevice: inject the Vulkan objects selected by the application startup
// path. create() consumes these handles; ownership remains with VulkanContext,
// so this adapter must never destroy the device or queue itself.
void Fsr4Int8Backend::setDevice(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                 uint32_t queueFamily, const GpuCapability& cap) {
    physical_ = physical; device_ = device; queue_ = queue;
    queueFamily_ = queueFamily; cap_ = cap;
}

// setWeightBlob: retain the validated weight view that create() uploads once
// into the harness. The blob must outlive create(); the harness owns its GPU
// copy after upload.
void Fsr4Int8Backend::setWeightBlob(const Fsr4BlobView& b) { blob_ = b; }

// create: initialize the GPU harness, allocate dimensions-specific resources,
// and upload weights. Called by BackendSelector when the source/preset changes.
// Failure is intentionally reported without inventing a CPU result: the
// selector owns the downstream fallback decision.
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

// reconfigure: replace every dimension-dependent GPU resource for a new
// source/preset target. Destroying first prevents old history or descriptors
// from leaking into the new stream; BackendSelector calls this on scale changes.
bool Fsr4Int8Backend::reconfigure(const UpscaleContextDesc& desc) {
    destroy();
    return create(desc);
}

// dispatch: submit one frame's temporal work to the harness and translate its
// result into the backend-neutral packet. The packet carries reset semantics
// from PlaybackEngine, so a seek or scene cut invalidates history before the
// next frame is considered temporally valid.
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

// resetHistory: force a reset dispatch without presenting its result. The
// playback path uses this at discontinuities so the next real frame cannot
// blend with pixels from the previous scene.
void Fsr4Int8Backend::resetHistory() {
    if (harness_) harness_->dispatchFrame(/*reset=*/true);
    info_.resetCount++;
}

// destroy: release the harness-owned Vulkan resources. Device/queue ownership
// remains with the application, and BackendSelector may call this repeatedly.
void Fsr4Int8Backend::destroy() {
    harness_.reset();
}

} // namespace temporal_forge
