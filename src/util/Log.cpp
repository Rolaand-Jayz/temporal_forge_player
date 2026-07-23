// Log.cpp
#include "Log.hpp"

#include <cstdio>

namespace temporal_forge {

Logger& Logger::instance() {
    static Logger singleton;
    // Make stdout/stderr line-buffered so logs survive a SIGTERM/kill (e.g.
    // timeout(1)) instead of sitting in a 4KiB pipe buffer.
    static bool once = [] {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        return true;
    }();
    (void)once;
    return singleton;
}

} // namespace temporal_forge
