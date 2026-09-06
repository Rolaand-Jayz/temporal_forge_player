// Fsr4PostpassParams.hpp — typed access to the recovered v4.1 postpass zone.
//
// Upstream: one validated 131072-byte FSR 4.1 weight blob. Downstream: the
// diagnostic postpass parameter trace and future host-side binding metadata.
// This module does not apply parameters to production composition by itself.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace temporal_forge {

// The recovered 4.1-only region begins immediately after the quantized tensor
// data and ends immediately before the documented zero-padding region.
constexpr size_t kFsr4PostpassParamOffset = 130088;
constexpr size_t kFsr4PostpassParamCount = 222;
constexpr size_t kFsr4PostpassParamBytes =
    kFsr4PostpassParamCount * sizeof(float);

using Fsr4PostpassParams = std::array<float, kFsr4PostpassParamCount>;

// Decode exactly the recovered little-endian FP32 region. A missing byte or a
// non-finite value is rejected so diagnostics cannot turn corrupt data into a
// plausible visual result.
std::optional<Fsr4PostpassParams> decodeFsr4PostpassParams(
    const uint8_t *blob, size_t blobSize);

} // namespace temporal_forge
