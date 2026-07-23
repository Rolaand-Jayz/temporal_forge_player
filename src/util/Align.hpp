// Align.hpp — small alignment helpers (spec 02: align_even, prefer 8px)
#pragma once
#include <cstdint>

namespace temporal_forge {

// isEven: tests whether v is divisible by 2.
//
// Called by: alignEven (this file).
// Notes:     constexpr; usable in constant expressions and template args.
constexpr bool isEven(uint32_t v) { return (v & 1u) == 0u; }

// alignEven: rounds v up to the next even value (minimum 2px alignment).
//
// Called by: fsrTargetSize() in FsrTargetMath.hpp, nativeInt8UltraPerformanceTarget(),
//            and the size computations in PlaybackEngine that must honor spec 02's
//            "FSR target dimensions must be even" rule.
// Calls:     isEven.
// Notes:     spec 02: minimum 2px alignment (dimensions must be even).
constexpr uint32_t alignEven(uint32_t v) {
    return isEven(v) ? v : v + 1u;
}

// alignTo: rounds v up to the next multiple of alignment.
//
// Called by: fsrTargetSize() (when a caller requests 8px or backend-specific
//            alignment beyond the default 2px).
// Notes:     spec 02: preferred 8px alignment. alignment==0 is a no-op (returns v).
constexpr uint32_t alignTo(uint32_t v, uint32_t alignment) {
    if (alignment == 0) return v;
    return ((v + alignment - 1u) / alignment) * alignment;
}

} // namespace temporal_forge




