// Fsr4PostpassParams.cpp — validate and decode the recovered postpass region.
//
// Upstream: raw bytes from WeightBlobLoader. Downstream: CPU diagnostics and
// the explicit postpass contract; no production image path calls this decoder
// until its values have passed the finite/range checks here.
#include "backend/Fsr4PostpassParams.hpp"

#include <cmath>
#include <cstring>

namespace temporal_forge {

std::optional<Fsr4PostpassParams> decodeFsr4PostpassParams(
    const uint8_t *blob, size_t blobSize) {
  if (!blob || blobSize < kFsr4PostpassParamOffset + kFsr4PostpassParamBytes)
    return std::nullopt;

  Fsr4PostpassParams result{};
  for (size_t index = 0; index < result.size(); ++index) {
    const size_t byteOffset = kFsr4PostpassParamOffset + index * sizeof(float);
    uint32_t bits = static_cast<uint32_t>(blob[byteOffset + 0]) |
                    (static_cast<uint32_t>(blob[byteOffset + 1]) << 8u) |
                    (static_cast<uint32_t>(blob[byteOffset + 2]) << 16u) |
                    (static_cast<uint32_t>(blob[byteOffset + 3]) << 24u);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value))
      return std::nullopt;
    result[index] = value;
  }
  return result;
}

} // namespace temporal_forge
