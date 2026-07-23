// Fsr4Int8Backend.hpp — the experimental FSR 4.1 INT8 RDNA3 backend.
//
// Wraps the Fsr4DispatchHarness (the 27-pass RE-derived neural upscaler) in
// the ITemporalUpscalerBackend interface. On RDNA3 (RX 7000), this is the
// default experimental path per the v3→10 policy doc.
//
// EXPERIMENTAL — RE-derived reimplementation. Proof-gated: the backend
// initializes only after the Fsr4ProofRunner validates the dispatch
// produces structurally-sane output. The project does not require comparison
// against AMD's Windows runtime or an official video-FSR reference.
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"
#include "backend/GpuCapabilityProbe.hpp"
#include "backend/WeightBlob.hpp"

#include <vulkan/vulkan.h>
#include <memory>
#include <string>

namespace temporal_forge {

class Fsr4DispatchHarness;

class Fsr4Int8Backend : public ITemporalUpscalerBackend {
public:
    Fsr4Int8Backend();
    ~Fsr4Int8Backend() override;

    [[nodiscard]] BackendInfo info() const override { return info_; }
    bool create(const UpscaleContextDesc& desc) override;
    bool reconfigure(const UpscaleContextDesc& desc) override;
    UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) override;
    void resetHistory() override;
    void destroy() override;

    // The GPU must be probed before create(); this stores the capability
    // the backend will use to initialize the dispatch harness.
    void setDevice(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                   uint32_t queueFamily, const GpuCapability& cap);
    void setWeightBlob(const Fsr4BlobView& blob);

    [[nodiscard]] bool ready() const;

private:
    BackendInfo info_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = ~0u;
    GpuCapability cap_;
    Fsr4BlobView blob_;
    std::unique_ptr<Fsr4DispatchHarness> harness_;
};

} // namespace temporal_forge
