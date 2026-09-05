// temporal_frame_continuity_tests.cpp — temporal state frame-pairing contract.
//
// The FSR history and recurrent textures describe the last frame that was
// successfully dispatched. This test locks the small state machine that
// decides whether a newly decoded frame may consume that state. It is kept
// independent of Vulkan so the frame-pairing rule can be verified before the
// decode loop uses it.
#include "util/TemporalFrameContinuity.hpp"

#include <cstdio>
#include <limits>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static void test_first_frame_requires_reset() {
    TemporalFrameContinuity continuity;
    CHECK(continuity.needsReset(0));
    continuity.commit(0);
    CHECK(!continuity.needsReset(1));
}

static void test_skipped_frame_requires_reset() {
    TemporalFrameContinuity continuity;
    continuity.commit(10);
    CHECK(!continuity.needsReset(11));
    CHECK(continuity.needsReset(12));
    CHECK(continuity.needsReset(10));
}

static void test_flush_forgets_old_state() {
    TemporalFrameContinuity continuity;
    continuity.commit(42);
    continuity.clear();
    CHECK(continuity.needsReset(43));
    continuity.commit(43);
    CHECK(!continuity.needsReset(44));
}

static void test_commit_happens_only_when_caller_accepts_dispatch() {
    TemporalFrameContinuity continuity;
    continuity.commit(20);
    // A failed dispatch must leave frame 20 as the last valid state. The
    // caller decides whether to commit; no implicit advancement is allowed.
    CHECK(continuity.needsReset(22));
    CHECK(!continuity.needsReset(21));
}

static void test_counter_overflow_is_never_treated_as_contiguous() {
    TemporalFrameContinuity continuity;
    continuity.commit(std::numeric_limits<uint64_t>::max());
    CHECK(continuity.needsReset(0));
}

int main() {
    test_first_frame_requires_reset();
    test_skipped_frame_requires_reset();
    test_flush_forgets_old_state();
    test_commit_happens_only_when_caller_accepts_dispatch();
    test_counter_overflow_is_never_treated_as_contiguous();
    return g_failures == 0 ? 0 : 1;
}
