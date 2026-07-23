// Fsr4TensorMap.hpp — parsed view of the RE's legacy tensor-map.json.
//
// The checked-in JSON is the 4.0.2-derived schema reference. The active v4.1
// dispatch path does not use these offsets: its physical weight/bias map is
// recovered from the v4.1 blob and lives next to the dispatch table.
//
// Tensor types (from the 4.0.2 HLSL source the RE references):
//   Tensor4h_HNWC<BufferStorage>  — 4D FP16/uint8 weight, HWNC layout
//   Tensor1f<BufferStorage>       — 1D FP32 bias
//   Tensor1h<BufferStorage>       — 1D FP16 bias
//
// The dispatch harness reads the relevant tensor's byte offset + shape when
// building each pass's constant buffer (CBV slot, RE §4.4).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace temporal_forge {

struct Fsr4Tensor {
    std::string pass;       // "0".."12" or "postpass" (encoder/decoder stage id)
    std::string name;       // e.g. encoder2_ResidualBlock_0_body_conv_dw_bias
    uint32_t offset = 0;    // byte offset within the 131072-B blob
    std::string shape;      // "2, 2, 7, 16" (raw)
    std::string storageSize;
    std::string tensorType; // Tensor4h_HNWC< BufferStorage > etc.
    uint32_t byteSize = 0;
    // Parsed shape dims (may be empty for 1D).
    std::vector<uint32_t> dims;
    // Zone: which region of the blob this tensor lives in.
    enum class Zone { BiasFp16, WeightUint8, ScaleFp16 } zone = Zone::BiasFp16;
};

struct Fsr4LayerInfo {
    // The 12 core ML passes + their residual-block structure.
    int passIndex = 0;              // 1..12
    std::string stage;              // encoder2/encoder3/bottleneck/decoder3/decoder2
    int channelsIn = 0;
    int channelsOut = 0;
    int kernelW = 0, kernelH = 0;   // conv kernel size (depthwise)
    std::string layerType;          // depthwise / pointwise_expand / pointwise_contract / spatial_mixing
    bool hasRelu = true;
};

class Fsr4TensorMap {
public:
    // Load + parse the tensor-map.json from the RE dataset.
    bool loadFromJson(const std::string& path);

    // All 78 tensors.
    [[nodiscard]] const std::vector<Fsr4Tensor>& tensors() const { return tensors_; }

    // Find a tensor by name substring (e.g. "encoder2_ResidualBlock_0_body_conv_dw_bias").
    [[nodiscard]] const Fsr4Tensor* find(const std::string& nameContains) const;

    // All tensors belonging to a given pass index ("0".."12", "postpass").
    [[nodiscard]] std::vector<const Fsr4Tensor*> tensorsForPass(const std::string& pass) const;

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] size_t count() const { return tensors_.size(); }

private:
    std::vector<Fsr4Tensor> tensors_;
    bool loaded_ = false;
};

// Hand-rolled tiny JSON value extractor for tensor-map.json (the schema is
// stable and flat). Avoids pulling a JSON dependency for one parse.
std::vector<Fsr4Tensor> parseTensorMapJson(const std::string& body);

} // namespace temporal_forge
