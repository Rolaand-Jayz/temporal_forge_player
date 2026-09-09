// PlaylistAdvancement.cpp — see PlaylistAdvancement.hpp.

#include "core/PlaylistAdvancement.hpp"

namespace temporal_forge {

bool shouldAdvancePlaylistAtEnd(const EndOfStreamState& state) {
    const bool audioStillPlaying =
        state.audioClockUs >= 0 && state.bufferedAudioFrames > 0;

    if (state.videoDrained && !audioStillPlaying) {
        // The decoder drained its end-of-stream and the final frame is on
        // screen: no future video PTS exists and no audio remains. Waiting
        // for a fixed allowance past the last PTS would stall forever when
        // that final frame legitimately spans more than the allowance (e.g.
        // low-frame-rate content), so advancement fires on drained state.
        return true;
    }

    // Audio is still playing (or video frames are still arriving): pace on
    // whichever clock has progressed furthest, and use the tail slack only
    // as jitter tolerance against the container timeline.
    int64_t pacedUs = -1;
    if (state.audioClockUs >= 0)
        pacedUs = state.audioClockUs;
    if (state.lastRenderedPtsUs > pacedUs)
        pacedUs = state.lastRenderedPtsUs;
    if (pacedUs < 0)
        return false; // nothing has been displayed yet

    const int64_t endUs = state.durationUs > 0 ? state.durationUs : pacedUs;
    if (pacedUs + kEosTailSlackUs < endUs)
        return false;
    if (state.durationUs <= 0 && state.audioClockUs >= 0 &&
        state.bufferedAudioFrames > 0)
        return false; // unknown duration: wait for the buffered audio tail
    return true;
}

} // namespace temporal_forge
