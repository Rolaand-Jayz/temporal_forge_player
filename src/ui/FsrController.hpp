// FsrController.hpp — Phase 2 bridge exposing the fixed scaling model to QML.
//
// spec 02: the FSR target depends ONLY on source size + preset. It is
// independent of window size. Window resize changes presentation only.
//
// Exposes:
//   - the preset list with per-preset computed target sizes (so QML can
//     show "Performance 2.0x — 1920x1080 -> 3840x2160")
//   - the active preset / backend
//   - the resolved "source -> FSR target -> window" string for the info panel
//
// Phase 3 will connect the backend selection to a real Fsr23SdkBackend.
#pragma once
#include "backend/UpscaleTypes.hpp"
#include "config/SettingsStore.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace temporal_forge {

class PlaybackEngine;

class FsrController : public QObject {
    Q_OBJECT
    Q_PROPERTY(UpscalePreset preset READ preset WRITE setPreset NOTIFY presetChanged)
    Q_PROPERTY(BackendKind backend READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(int sourceWidth READ sourceWidth NOTIFY sourceChanged)
    Q_PROPERTY(int sourceHeight READ sourceHeight NOTIFY sourceChanged)
    Q_PROPERTY(int fsrTargetWidth READ fsrTargetWidth NOTIFY scalingChanged)
    Q_PROPERTY(int fsrTargetHeight READ fsrTargetHeight NOTIFY scalingChanged)
    Q_PROPERTY(QString presetLabel READ presetLabel NOTIFY presetChanged)
    Q_PROPERTY(QString backendLabel READ backendLabel NOTIFY backendChanged)
    Q_PROPERTY(QString chainLabel READ chainLabel NOTIFY scalingChanged)
    Q_PROPERTY(QVariantList presetOptions READ presetOptions CONSTANT)
    Q_PROPERTY(QVariantList backendOptions READ backendOptions CONSTANT)

public:
    explicit FsrController(PlaybackEngine* engine, QObject* parent = nullptr);

    // spec 02 enums exposed to QML as raw ints via Q_ENUM
    Q_ENUM(UpscalePreset)
    Q_ENUM(BackendKind)

    UpscalePreset preset() const { return preset_; }
    void setPreset(UpscalePreset p);
    BackendKind backend() const { return backend_; }
    void setBackend(BackendKind b);

    int sourceWidth() const { return srcW_; }
    int sourceHeight() const { return srcH_; }
    int fsrTargetWidth() const;
    int fsrTargetHeight() const;
    QString presetLabel() const;
    QString backendLabel() const;
    QString chainLabel() const; // "1920x1080 -> FSR Performance 3840x2160 -> Window WxH"

    // For window size tracking (set from QML on resize).
    Q_INVOKABLE void setWindowSize(int w, int h);

    QVariantList presetOptions() const;  // [{key, label, target, ratio}, ...]
    QVariantList backendOptions() const;

signals:
    void presetChanged();
    void backendChanged();
    void sourceChanged();
    void scalingChanged();

private:
    void refreshSource();

    PlaybackEngine* engine_;
    UpscalePreset preset_ = UpscalePreset::Quality;
    BackendKind backend_ = BackendKind::Fsr4ReExperimental;
    int srcW_ = 0, srcH_ = 0;
    int winW_ = 1280, winH_ = 720;
};

} // namespace temporal_forge
