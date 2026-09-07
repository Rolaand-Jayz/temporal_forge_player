// file_switch_tests.cpp — verifies the PlaybackEngine gracefully switches
// files and tears down/recreates the FSR4 path while playback is running.
//
// This exercises the specific bug the user reported: opening a new file
// after playback has started used to deadlock because openUrl called both
// close() and stopThreads() (the latter joining a decode thread that was
// blocked inside vkWaitForFences), and a stale seekPending_ flag survived
// the close, leaving the new demux thread seeking on a fresh file.
//
// The test runs without a GPU: setVulkanHandles is never called, so the
// FSR4 path stays disabled and the engine plays raw decoded frames. That
// is enough to exercise the thread join / seek-flag / media-reopen path
// that was broken.
#include "core/PlaybackEngine.hpp"

#include <QCoreApplication>
#include <QUrl>
#include <QTimer>
#include <QFileInfo>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace temporal_forge;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static bool has_media_within(PlaybackEngine &engine, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i += 50) {
        if (engine.hasMedia()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return engine.hasMedia();
}

static bool playlist_reaches_index(PlaybackEngine &engine, int wanted,
                                   int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 25) {
        // A real QML VideoSurface consumes frames through this method. Pump
        // it here as well so EOF advancement is tested with the same queue
        // state as the player, without requiring a GUI window in ctest.
        (void)engine.advanceRenderFrame();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (engine.playlistIndex() == wanted)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return engine.playlistIndex() == wanted;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // Resolve two sample files. We prefer the generated sample.mp4 (always
    // available in the build/tests dir) and any corpus clip as the second.
    std::string fileA, fileB;
    if (const char *env = std::getenv("TFORGE_SAMPLE")) fileA = env;
    else if (argc > 1) fileA = argv[1];
    else fileA = "sample.mp4";
    if (argc > 2) fileB = argv[2];
    else if (const char *env = std::getenv("TFORGE_SAMPLE_B")) fileB = env;
    else fileB = "";

    if (!std::filesystem::exists(fileA)) {
        std::fprintf(stderr, "file_switch_tests: SKIP (sample %s not found)\n", fileA.c_str());
        return 77;
    }
    if (fileB.empty() || !std::filesystem::exists(fileB)) {
        std::fprintf(stderr, "file_switch_tests: SKIP (second sample not found)\n");
        return 77;
    }

    PlaybackEngine engine;

    // 1. Open both files as one playlist and wait for the first item.
    // This exercises the public playlist API before the legacy single-file
    // switch checks below. The fixtures intentionally may be identical; the
    // playlist index and transport state are the observable contract here.
    engine.openPlaylist(QStringList{
        QString::fromStdString(fileA), QString::fromStdString(fileB)});
    CHECK(engine.playlistCount() == 2);
    CHECK(engine.playlistIndex() == 0);
    CHECK(engine.hasNext());
    CHECK(!engine.hasPrevious());
    CHECK(has_media_within(engine, 3000));
    CHECK(engine.mediaTitle().toStdString().find(
        QFileInfo(QString::fromStdString(fileA)).fileName().toStdString()) != std::string::npos);

    // Manual playlist navigation must use the same clean close/open path as
    // an automatic end-of-media transition.
    engine.next();
    CHECK(engine.playlistIndex() == 1);
    CHECK(!engine.hasNext());
    CHECK(engine.hasPrevious());
    CHECK(has_media_within(engine, 3000));
    engine.previous();
    CHECK(engine.playlistIndex() == 0);
    CHECK(has_media_within(engine, 3000));

    // 2. Let it actually decode + play for ~500ms so threads are mid-flight.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 3. Open the second file. This used to deadlock because openUrl called
    //    stopThreads() twice (once via close(), once directly) and the
    //    decode thread was blocked inside the FSR4 dispatch's fence wait.
    //    Now it should switch cleanly within a couple of seconds.
    engine.openUrl(QUrl::fromLocalFile(QString::fromStdString(fileB)));
    CHECK(has_media_within(engine, 3000));
    CHECK(engine.mediaTitle().toStdString().find(
        QFileInfo(QString::fromStdString(fileB)).fileName().toStdString()) != std::string::npos);

    // 4. Switch back to file A and immediately toggle play state. This
    //    exercises the close+open+toggle ordering with no idle time between.
    engine.openUrl(QUrl::fromLocalFile(QString::fromStdString(fileA)));
    engine.togglePlay();
    CHECK(has_media_within(engine, 3000));

    // 5. Switch back to file B once more, then close. The close path itself
    //    used to leave stale state (seekPending_) that broke the next open.
    engine.openUrl(QUrl::fromLocalFile(QString::fromStdString(fileB)));
    CHECK(has_media_within(engine, 3000));
    engine.close();
    CHECK(!engine.hasMedia());

    // 6. After a close, a fresh open must still work (no stale seek).
    engine.openUrl(QUrl::fromLocalFile(QString::fromStdString(fileA)));
    CHECK(has_media_within(engine, 3000));
    engine.close();

    // 7. End-of-media should advance a playing playlist item automatically.
    // The generated fixture is five seconds long, so this remains a bounded
    // integration check rather than a synthetic signal-only test.
    engine.openPlaylist(QStringList{
        QString::fromStdString(fileA), QString::fromStdString(fileB)});
    CHECK(playlist_reaches_index(engine, 1, 7500));
    CHECK(engine.playlistIndex() == 1);
    CHECK(engine.hasMedia());
    engine.close();

    if (g_failures == 0) {
        std::printf("file_switch_tests: PASS (playlist navigation, EOF advance, and clean file switches)\n");
        return 0;
    }
    std::printf("file_switch_tests: FAIL (%d failures)\n", g_failures);
    return 1;
}
