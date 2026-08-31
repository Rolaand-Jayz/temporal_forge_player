// TemporalFrameContinuity.hpp — guards persistent temporal state by frame ID.
//
// FSR history and recurrent state are valid only for the immediately
// preceding frame that completed the dispatch. PlaybackEngine owns the actual
// GPU state and calls this small value type before consuming it and after a
// successful dispatch. The type has no Vulkan or image-quality behavior; it
// only makes frame pairing explicit and unit-testable.
#pragma once

#include <cstdint>
#include <limits>

namespace temporal_forge {

class TemporalFrameContinuity {
public:
  // Return true when the current frame cannot safely consume retained state.
  // The first frame, a skipped/reordered frame, and a repeated frame all
  // require the caller to use the FSR reset path.
  [[nodiscard]] bool needsReset(uint64_t currentFrameIndex) const {
    return !hasCommittedFrame_ ||
           currentFrameIndex == std::numeric_limits<uint64_t>::max() ||
           lastCommittedFrameIndex_ == std::numeric_limits<uint64_t>::max() ||
           currentFrameIndex != lastCommittedFrameIndex_ + 1;
  }

  // Record a frame only after its complete temporal dispatch succeeds. A
  // failed submission therefore cannot advance the state pairing contract.
  void commit(uint64_t frameIndex) {
    hasCommittedFrame_ = true;
    lastCommittedFrameIndex_ = frameIndex;
  }

  // Forget the pairing after seek, flush, close, or any explicit state reset.
  void clear() {
    hasCommittedFrame_ = false;
    lastCommittedFrameIndex_ = 0;
  }

private:
  bool hasCommittedFrame_ = false;
  uint64_t lastCommittedFrameIndex_ = 0;
};

} // namespace temporal_forge
