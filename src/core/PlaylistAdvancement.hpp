// PlaylistAdvancement.hpp — pure end-of-stream advancement decision.
//
// Extracted from PlaybackEngine so the EOF/playlist transition is unit
// testable without a media pipeline. The decision is based on actual
// decoded/drained stream state plus the real presentation timeline, not a
// universal fixed tail assumption.

#pragma once

#include <cstddef>
#include <cstdint>

namespace temporal_forge {

// Slack for clock-vs-container jitter while an audio clock is genuinely
// advancing. This is pacing slack only — never the sole gate for advancing,
// which is what made long final frames stall under the previous logic.
inline constexpr int64_t kEosTailSlackUs = 250'000;

struct EndOfStreamState {
    int64_t durationUs = -1;        // container duration; <= 0 when unknown
    int64_t lastRenderedPtsUs = -1; // PTS of the final displayed video frame
    int64_t audioClockUs = -1;      // < 0 when no audio clock exists
    size_t bufferedAudioFrames = 0; // audio samples still queued for playback
    bool videoDrained = false;      // decoder drained its EOF; frame queue empty
};

// True when playlist advancement must fire. Driven by real stream state:
// once the video decoder has drained and the final frame is displayed, no
// future video PTS exists; advancement then waits only for audio that is
// still actually playing (preserving the audio-tail pacing fix).
bool shouldAdvancePlaylistAtEnd(const EndOfStreamState& state);

} // namespace temporal_forge
