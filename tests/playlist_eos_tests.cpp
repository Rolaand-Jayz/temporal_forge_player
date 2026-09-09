// playlist_eos_tests.cpp — end-of-stream advancement decision contract.
//
// Regression coverage for the playlist EOF transition: advancement must be
// driven by decoded/drained stream state and the real presentation timeline,
// never by a universal fixed tail allowance. The previous fixed 250 ms gate
// stalled indefinitely on content whose final video frame legitimately spans
// more than 250 ms (low-frame-rate media).

#include "core/PlaylistAdvancement.hpp"

#include <cstdint>
#include <cstdio>

using temporal_forge::EndOfStreamState;
using temporal_forge::shouldAdvancePlaylistAtEnd;

namespace {

int failures = 0;

void expectAdvance(const EndOfStreamState& state, bool wantAdvance) {
    const bool got = shouldAdvancePlaylistAtEnd(state);
    if (got != wantAdvance) {
        std::fprintf(stderr,
                     "FAIL: want advance=%d, got %d (duration=%lld lastPts=%lld "
                     "audioClock=%lld buffered=%zu drained=%d)\n",
                     wantAdvance ? 1 : 0, got ? 1 : 0,
                     static_cast<long long>(state.durationUs),
                     static_cast<long long>(state.lastRenderedPtsUs),
                     static_cast<long long>(state.audioClockUs),
                     state.bufferedAudioFrames,
                     state.videoDrained ? 1 : 0);
        ++failures;
    }
}

} // namespace

int main() {
    // 1. Ordinary frame cadence (video + audio, both near the end): the
    //    advancing audio clock plus tail slack reaches the container end.
    expectAdvance(
        {.durationUs = 60'000'000, .lastRenderedPtsUs = 59'500'000,
         .audioClockUs = 59'900'000, .bufferedAudioFrames = 100,
         .videoDrained = true},
        true);
    expectAdvance(
        {.durationUs = 60'000'000, .lastRenderedPtsUs = 59'500'000,
         .audioClockUs = 59'000'000, .bufferedAudioFrames = 100,
         .videoDrained = true},
        false);

    // 2. Long final video frame: video-only content whose final frame spans
    //    2 s. lastPts freezes at 60 s while the container ends at 62 s. The
    //    old fixed-250 ms gate stalled here forever; drained-state semantics
    //    must advance once the final frame is displayed.
    expectAdvance(
        {.durationUs = 62'000'000, .lastRenderedPtsUs = 60'000'000,
         .audioClockUs = -1, .bufferedAudioFrames = 0,
         .videoDrained = true},
        true);
    // ...but only after the decoder actually drained: the final frame must
    // stay on screen, not be cut off by the PTS gap.
    expectAdvance(
        {.durationUs = 62'000'000, .lastRenderedPtsUs = 60'000'000,
         .audioClockUs = -1, .bufferedAudioFrames = 0,
         .videoDrained = false},
        false);

    // 3. Audio longer than video: video drained, audio clock still advancing
    //    below the end. Audio owns the tail (previous fix preserved).
    expectAdvance(
        {.durationUs = 70'000'000, .lastRenderedPtsUs = 60'000'000,
         .audioClockUs = 65'000'000, .bufferedAudioFrames = 500,
         .videoDrained = true},
        false);
    expectAdvance(
        {.durationUs = 70'000'000, .lastRenderedPtsUs = 60'000'000,
         .audioClockUs = 69'900'000, .bufferedAudioFrames = 20,
         .videoDrained = true},
        true);

    // 4. Video longer than audio: the audio track drained early (buffered
    //    frames exhausted, clock frozen mid-stream). Once the video decoder
    //    drains and the final frame is displayed, advancement must fire even
    //    though the frozen audio clock never approaches the container end.
    expectAdvance(
        {.durationUs = 60'000'000, .lastRenderedPtsUs = 59'000'000,
         .audioClockUs = 30'000'000, .bufferedAudioFrames = 0,
         .videoDrained = true},
        true);
    // ...and before the video drains, presentation continues: no advance.
    expectAdvance(
        {.durationUs = 60'000'000, .lastRenderedPtsUs = 59'000'000,
         .audioClockUs = 30'000'000, .bufferedAudioFrames = 0,
         .videoDrained = false},
        false);

    // 5. Unknown container duration with a buffered audio tail: keep waiting
    //    for the audible tail (existing contract preserved).
    expectAdvance(
        {.durationUs = -1, .lastRenderedPtsUs = 60'000'000,
         .audioClockUs = 60'100'000, .bufferedAudioFrames = 50,
         .videoDrained = true},
        false);

    // 6. Nothing displayed yet: never advance.
    expectAdvance(
        {.durationUs = 60'000'000, .lastRenderedPtsUs = -1,
         .audioClockUs = 1'000'000, .bufferedAudioFrames = 10,
         .videoDrained = false},
        false);

    if (failures != 0) {
        std::fprintf(stderr, "playlist_eos_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("playlist_eos_tests: all checks passed");
    return 0;
}
