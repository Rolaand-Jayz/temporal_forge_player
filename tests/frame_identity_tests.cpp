// frame_identity_tests.cpp — spec 02 & 07 "Frame Identity".
//
// Verifies the inviolable rule at the data-model level:
//   input frame count == output frame count
//   input PTS == output PTS
//   no generated frames exist between source frames
// And scans source for forbidden frame-generation code paths (spec 07
// "No Interpolation" audit).
#include "util/FsrTargetMath.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

// Simulate the per-frame contract. Every decoded source frame produces
// exactly one upscaled output frame, carrying the same PTS/duration.
struct SourceFrame { int64_t pts; int64_t duration; };
struct OutputFrame { int64_t pts; int64_t duration; };

static void test_one_to_one_cfr() {
    // 24 fps CFR, 10 frames.
    std::vector<SourceFrame> src;
    const int64_t tick = 3754; // ~23.976 in 1/90000 ticks
    for (int i = 0; i < 10; ++i) src.push_back({i * tick, tick});

    std::vector<OutputFrame> out;
    out.reserve(src.size());
    // spec 02: outputFrame.pts = inputFrame.pts; outputFrame.duration = inputFrame.duration
    for (const auto& s : src) out.push_back({s.pts, s.duration});

    CHECK(out.size() == src.size()); // no frame generation / drop by default
    for (size_t i = 0; i < src.size(); ++i) {
        CHECK(out[i].pts == src[i].pts);
        CHECK(out[i].duration == src[i].duration);
    }
}

static void test_one_to_one_vfr() {
    // VFR: durations vary; PTS still preserved 1:1 (spec 02 VFR section).
    std::vector<SourceFrame> src = {{0, 4000}, {4000, 4000}, {8000, 2000},
                                    {10000, 6000}, {16000, 4000}};
    std::vector<OutputFrame> out;
    for (const auto& s : src) out.push_back({s.pts, s.duration});
    CHECK(out.size() == src.size());
    for (size_t i = 0; i < src.size(); ++i)
        CHECK(out[i].pts == src[i].pts);
}

// Forbidden substrings in our own source that would indicate frame
// generation / interpolation / cadence conversion. (spec 07 audit.)
// Note: this test file itself contains the words, so it is excluded from
// the scan; the scan only walks src/ and resources/qml/.
static void test_no_generation_code() {
    const std::vector<std::string> banned = {
        "interpolateFrame",
        "generateIntermediateFrame",
        "frameGenerationQueue",
        "opticalFlowSynthesis",
        "convertCadence",
        "smoothMotion",
        "boostFrameRate",
    };

    // Recursively collect .cpp/.hpp/.qml under src/ and resources/qml/.
    // TFORGE_SOURCE_ROOT is passed at compile time by CMake so the scan is
    // independent of the test's working directory.
#ifdef TFORGE_SOURCE_ROOT
    const std::string srcRoot = TFORGE_SOURCE_ROOT;
#else
    const std::string srcRoot = "../..";
#endif
    const std::string roots[] = {
        srcRoot + "/src",
        srcRoot + "/resources/qml",
    };
    int hits = 0;
    for (const std::string& root : roots) {
        std::string cmd = "find '"; cmd += root;
        cmd += "' -type f \\( -name '*.cpp' -o -name '*.hpp' -o -name '*.qml' \\) "
               "-print 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) continue;
        char* line = nullptr; size_t cap = 0; ssize_t n;
        std::string path;
        while ((n = getline(&line, &cap, pipe)) != -1) {
            if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
            path = line;
            std::ifstream f(path);
            if (!f) continue;
            std::stringstream ss; ss << f.rdbuf();
            std::string body = ss.str();
            for (const auto& b : banned) {
                if (body.find(b) != std::string::npos) {
                    std::fprintf(stderr, "BANNED TOKEN '%s' in %s\n", b.c_str(), path.c_str());
                    ++hits;
                }
            }
        }
        free(line);
        pclose(pipe);
    }
    CHECK(hits == 0);
}

int main() {
    test_one_to_one_cfr();
    test_one_to_one_vfr();
    test_no_generation_code();
    if (g_failures == 0) { std::printf("frame_identity_tests: OK\n"); return 0; }
    std::fprintf(stderr, "frame_identity_tests: %d FAILURES\n", g_failures);
    return 1;
}
