// DebugOverlayModel.hpp — spec 05 "Debug Overlay" fields.
// Exposes the per-frame telemetry the Ctrl+F overlay shows:
//   source resolution, FSR target, window res, backend, preset, video FPS,
//   present FPS, frame PTS, decode/upload/color/motion/mask/FSR/present/UI
//   times, dropped frames, history resets, scene cuts, motion confidence,
//   reactive average, VRAM estimate.
#pragma once
#include "backend/UpscaleTypes.hpp"
#include "config/SettingsStore.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace temporal_forge {

struct RenderStats {
    // Sizes
    int sourceW = 0, sourceH = 0;
    int fsrTargetW = 0, fsrTargetH = 0;
    int windowW = 0, windowH = 0;
    // Backend / preset
    BackendKind backend = BackendKind::Null;
    UpscalePreset preset = UpscalePreset::Off;
    double videoFps = 0.0;
    double presentFps = 0.0;
    // Timing (ms)
    double decodeMs = 0.0;
    double uploadMs = 0.0;
    double colorConvertMs = 0.0;
    double motionMs = 0.0;
    double maskMs = 0.0;
    double fsrDispatchMs = 0.0;
    double presentScaleMs = 0.0;
    double uiCompositeMs = 0.0;
    // Counters
    int64_t framePtsUs = 0;
    uint64_t droppedFrames = 0;
    uint64_t historyResets = 0;
    uint64_t sceneCutsDetected = 0;
    float motionConfidence = 1.0f;
    float reactiveAverage = 0.0f;
    uint64_t vramEstimate = 0;
};

class DebugOverlayModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY statsChanged)
    Q_PROPERTY(QVariantMap stats READ statsQml NOTIFY statsChanged)

public:
    explicit DebugOverlayModel(QObject* parent = nullptr);

    bool visible() const { return visible_; }
    void setVisible(bool v);

    void update(const RenderStats& s);
    void mergePartial(const RenderStats& partial); // update only set fields
    [[nodiscard]] const RenderStats& raw() const { return stats_; }
    [[nodiscard]] QVariantMap statsQml() const;

signals:
    void statsChanged();

private:
    RenderStats stats_;
    bool visible_ = false;
};

} // namespace temporal_forge
