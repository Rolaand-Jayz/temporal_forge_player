// main.cpp — Phase 1 entry point.
// Wires: Vulkan context, settings, PlaybackEngine (QML bridge), and the
// native Vulkan video surface.
#include "config/SettingsStore.hpp"
#include "core/PlaybackEngine.hpp"
#include "render/VulkanContext.hpp"
#include "ui/VideoSurfaceItem.hpp"
#include "ui/VideoSettingsController.hpp"
#include "ui/CompareController.hpp"
#include "ui/FsrController.hpp"
#include "ui/DebugOverlayModel.hpp"
#include "ui/PostProcessController.hpp"
#include "ui/ScreenCaptureController.hpp"
#include "util/Log.hpp"

#include <QGuiApplication>
#include <QVulkanInstance>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickGraphicsDevice>
#include <QSGRendererInterface>
#include <QQuickItem>
#include <QTimer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QtGlobal>
#include <QtQml>

#include <algorithm>
#include <cstdlib>

// main: application entry point.
//
// Wires together the whole stack: Qt + Vulkan instance/context (shared with
// the FSR4 compute device), Settings persistence, the PlaybackEngine (QML
// `playback` context property), the native Vulkan VideoSurfaceItem QML type,
// and the six QML-facing controllers (fsr/debug/videoSettings/compare/
// postProcess/capture). Runs the Qt event loop until the window closes.
//
// Sequence:
//   1. Create QVulkanInstance + VulkanContext sharing the same instance as Qt.
//   2. Load + re-save Settings (normalize the on-disk file).
//   3. Construct PlaybackEngine + controllers; wire the FSR4 path to the device
//      when Vulkan initialized (engine.setVulkanHandles).
//   4. Register VideoSurfaceItem as a QML type and expose controllers as context
//      properties.
//   5. Start a 60Hz-capped debug-overlay refresh timer (frame pulling itself is
//      driven from QML — VideoSurface.refresh() — to avoid starvation).
//   6. exec() the Qt event loop.
//
// Returns 1 only if the Qt Vulkan instance cannot be created.
int main(int argc, char** argv) {
    temporal_forge::Logger::instance().setLevel(temporal_forge::LogLevel::Debug);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("Temporal Forge Player");
    QGuiApplication::setApplicationVersion("0.1.0");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QVulkanInstance qtVulkan;
    qtVulkan.setApiVersion(QVersionNumber(1, 3, 0));
    if (!qtVulkan.create()) {
        temporal_forge::logError("Qt Vulkan instance creation failed");
        return 1;
    }

    // Create the compute device from Qt's instance so Qt Quick and FSR4 share
    // the same physical-device and Vulkan instance identity.
    temporal_forge::VulkanContext vk;
    const bool wantValidation = std::getenv("TFORGE_VK_VALIDATE") != nullptr;
    if (!vk.init(wantValidation, qtVulkan.vkInstance())) {
        temporal_forge::logWarn("Vulkan init failed; video playback unavailable.");
    }

    // Settings.
    temporal_forge::SettingsStore store(temporal_forge::SettingsStore::defaultPath());
    temporal_forge::Settings settings;
    const bool hadSettings = store.load(settings);
    temporal_forge::logInfo("Settings: {} (path: {})",
                            hadSettings ? "loaded" : "defaults",
                            store.path().string());
    store.save(settings);
    if (const char* value = std::getenv("TFORGE_BENCHMARK_SHARPNESS")) {
        settings.sharpness = std::clamp(std::strtof(value, nullptr), 0.0f, 1.0f);
    }
    if (const char* value = std::getenv("TFORGE_BENCHMARK_JITTER_STRENGTH")) {
        settings.jitterStrength = std::clamp(std::strtof(value, nullptr), 0.0f, 1.5f);
    }

    temporal_forge::PlaybackEngine engine;
    temporal_forge::FsrController fsr(&engine);
    temporal_forge::DebugOverlayModel debug;
    temporal_forge::VideoSettingsController videoSettings;
    temporal_forge::CompareController compare;
    temporal_forge::PostProcessController postProcess;
    temporal_forge::ScreenCaptureController capture;
    videoSettings.setSettings(&settings);
    engine.setSharpness(settings.sharpness);
    engine.setJitterStrength(settings.jitterStrength);
    QObject::connect(&videoSettings, &temporal_forge::VideoSettingsController::tuningChanged,
                     [&]() {
                         engine.setSharpness(videoSettings.sharpness());
                         engine.setJitterStrength(videoSettings.jitterStrength());
                     });

    // Wire the FSR4 real-frame upscaling path to the Vulkan device.
    if (vk.valid()) {
        // RADV's dedicated compute family is available, but this generated
        // graph is measurably faster on the universal graphics/compute queue
        // on the RX 7900 GRE. Keep presentation and FSR on that queue unless
        // a future asynchronous path proves otherwise.
        engine.setVulkanHandles(vk.physical(), vk.device(), vk.queue(),
                                vk.queueFamily(), vk.queueFamily());
        // FSR4 RE reconstruction: signed-INT8 tensor codepoints, the tensor
        // layout recovered from the local RE, and the native Vulkan postpass.
        // Validation is local to this RE-derived video implementation; no
        // Windows runtime or official video-FSR reference is a gate.
        // User selection in FsrController enables the real-frame FSR4 path.
    }
    fsr.setPreset(settings.preset);
    fsr.setBackend(settings.backend);

    // Register the native Vulkan video surface as a QML type. The surface
    // samples the image written by the real FSR4 dispatch; there is no CPU
    // image-provider path in the presentation chain.
    qmlRegisterType<temporal_forge::VideoSurfaceItem>(
        "TemporalForge", 1, 0, "VideoSurface");

    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty("playback", &engine);
    qml.rootContext()->setContextProperty("fsr", &fsr);
    qml.rootContext()->setContextProperty("debug", &debug);
    qml.rootContext()->setContextProperty("videoSettings", &videoSettings);
    qml.rootContext()->setContextProperty("compare", &compare);
    qml.rootContext()->setContextProperty("postProcess", &postProcess);
    qml.rootContext()->setContextProperty("capture", &capture);

    // Refresh loop: pull frames from the engine as fast as the UI thread can
    // paint. The engine's audio clock paces which frame is shown.
    QTimer refreshTimer;
    refreshTimer.setInterval(16); // ~60Hz cap; actual cadence follows source PTS
    QElapsedTimer fpsClock;
    fpsClock.start();
    qint64 lastFrameCount = 0;
    QObject::connect(&refreshTimer, &QTimer::timeout, [&]() {
        // NOTE: Frame pulling is now driven from QML (VideoSurface.refresh()
        // via the QML Timer in Main.qml). The old provider->refresh() path
        // competed for the same takeRenderFrame() call and starved the
        // VideoSurface. The C++ refreshTimer now only updates the debug
        // overlay telemetry.
        const int before = engine.frameCounter();
        (void)before;
        // Update debug overlay telemetry (spec 05).

        // Update debug overlay telemetry (spec 05).
        if (debug.visible()) {
            temporal_forge::RenderStats s;
            int sw = 0, sh = 0;
            engine.sourceDimensions(sw, sh);
            s.sourceW = sw; s.sourceH = sh;
            s.fsrTargetW = fsr.fsrTargetWidth();
            s.fsrTargetH = fsr.fsrTargetHeight();
            s.backend = engine.backendSelector().activeKind();
            s.preset = fsr.preset();
            s.historyResets = engine.historyResets();
            s.sceneCutsDetected = engine.sceneCutsDetected();
            s.motionConfidence = engine.lastMotionConfidence();
            s.reactiveAverage = engine.lastReactiveAverage();
            // Present FPS from frame counter deltas.
            const qint64 now = fpsClock.elapsed();
            const qint64 delta = now - lastFrameCount;
            (void)delta;
            debug.update(s);
        }
        lastFrameCount = fpsClock.elapsed();
    });

    QObject::connect(&qml, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    qml.loadFromModule("TemporalForge", "Main");
    if (qml.rootObjects().isEmpty()) {
        temporal_forge::logError("QML: failed to load Main");
        return 1;
    }

    // Restore window geometry from persisted settings (spec 05).
    if (auto* root = qml.rootObjects().first()) {
        if (auto* win = qobject_cast<QQuickWindow*>(root)) {
            win->setVulkanInstance(&qtVulkan);
            win->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
                vk.physical(), vk.device(), static_cast<int>(vk.queueFamily())));
            win->setGeometry(settings.windowX, settings.windowY,
                             settings.windowW, settings.windowH);
            engine.setVolume(settings.volume);
            engine.setMuted(false);
            const bool headlessBenchmark = std::getenv("TFORGE_HEADLESS_BENCHMARK") != nullptr;
            if (!headlessBenchmark) {
                if (settings.fullscreen) win->showFullScreen();
                else win->show();
            }
        }
    }

    refreshTimer.start();

    // If a file was passed on the command line, open it immediately.
    if (argc > 1) {
        const QString arg = QString::fromUtf8(argv[1]);
        QFileInfo fi(arg);
        QUrl url = fi.exists() ? QUrl::fromLocalFile(fi.absoluteFilePath())
                               : QUrl::fromUserInput(arg);
        temporal_forge::logInfo("Auto-opening file: {}", argv[1]);
        engine.openUrl(url);
    }

    // Persist settings on quit (spec 05 "Settings Persistence").
    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        temporal_forge::Settings s;
        store.load(s); // start from current
        s.preset = fsr.preset();
        s.backend = fsr.backend();
        s.brightness = videoSettings.brightness();
        s.contrast = videoSettings.contrast();
        s.saturation = videoSettings.saturation();
        s.hue = videoSettings.hue();
        s.gamma = videoSettings.gamma();
        s.sharpness = videoSettings.sharpness();
        s.jitterStrength = videoSettings.jitterStrength();
        s.volume = engine.volume();
        if (auto* root = qml.rootObjects().isEmpty() ? nullptr : qml.rootObjects().first()) {
            if (auto* win = qobject_cast<QQuickWindow*>(root)) {
                s.windowW = win->width();
                s.windowH = win->height();
                s.windowX = win->x();
                s.windowY = win->y();
                s.fullscreen = (win->visibility() == QWindow::FullScreen);
            }
        }
        store.save(s);
        temporal_forge::logInfo("Settings saved on exit.");
    });

    temporal_forge::logInfo("Temporal Forge Player: ready");
    return app.exec();
}
