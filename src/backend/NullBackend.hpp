// NullBackend.hpp — spec 04 section 4. Returns input color / simple copy.
// Used for pipeline bring-up, UI testing, frame pacing validation.
#pragma once
#include "backend/ITemporalUpscalerBackend.hpp"

namespace temporal_forge {

class NullBackend : public ITemporalUpscalerBackend {
public:
    [[nodiscard]] BackendInfo info() const override { return info_; }
    bool create(const UpscaleContextDesc& desc) override;
    bool reconfigure(const UpscaleContextDesc& desc) override;
    UpscaleOutputPacket dispatch(const VideoFsrPacket& packet) override;
    void resetHistory() override {}
    void destroy() override;

private:
    BackendInfo info_;
    bool created_ = false;
};

} // namespace temporal_forge
