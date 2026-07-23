// DebugOverlayModel.cpp
#include "ui/DebugOverlayModel.hpp"

namespace temporal_forge {

DebugOverlayModel::DebugOverlayModel(QObject* parent) : QObject(parent) {}

void DebugOverlayModel::setVisible(bool v) {
    if (v == visible_) return;
    visible_ = v;
    emit statsChanged();
}

void DebugOverlayModel::update(const RenderStats& s) {
    stats_ = s;
    emit statsChanged();
}

void DebugOverlayModel::mergePartial(const RenderStats& partial) {
    if (partial.framePtsUs) stats_.framePtsUs = partial.framePtsUs;
    if (partial.fsrDispatchMs) stats_.fsrDispatchMs = partial.fsrDispatchMs;
    if (partial.historyResets) stats_.historyResets = partial.historyResets;
    if (partial.sceneCutsDetected) stats_.sceneCutsDetected = partial.sceneCutsDetected;
    if (partial.motionConfidence != 1.0f) stats_.motionConfidence = partial.motionConfidence;
    if (partial.reactiveAverage) stats_.reactiveAverage = partial.reactiveAverage;
    stats_.droppedFrames = partial.droppedFrames ? partial.droppedFrames : stats_.droppedFrames;
    stats_.presentFps = partial.presentFps ? partial.presentFps : stats_.presentFps;
    emit statsChanged();
}

QVariantMap DebugOverlayModel::statsQml() const {
    QVariantMap m;
    m["sourceResolution"] = QString("%1x%2").arg(stats_.sourceW).arg(stats_.sourceH);
    m["fsrTargetResolution"] = QString("%1x%2").arg(stats_.fsrTargetW).arg(stats_.fsrTargetH);
    m["windowResolution"] = QString("%1x%2").arg(stats_.windowW).arg(stats_.windowH);
    m["backend"] = QString::fromUtf8(backendDisplayName(stats_.backend));
    m["preset"] = QString::fromUtf8(presetDisplayName(stats_.preset));
    m["videoFps"] = stats_.videoFps;
    m["presentFps"] = stats_.presentFps;
    m["framePts"] = QString::number(stats_.framePtsUs / 1e6, 'f', 3) + "s";
    m["decodeMs"] = stats_.decodeMs;
    m["uploadMs"] = stats_.uploadMs;
    m["colorConvertMs"] = stats_.colorConvertMs;
    m["motionMs"] = stats_.motionMs;
    m["maskMs"] = stats_.maskMs;
    m["fsrDispatchMs"] = stats_.fsrDispatchMs;
    m["presentScaleMs"] = stats_.presentScaleMs;
    m["uiCompositeMs"] = stats_.uiCompositeMs;
    m["droppedFrames"] = static_cast<qint64>(stats_.droppedFrames);
    m["historyResets"] = static_cast<qint64>(stats_.historyResets);
    m["sceneCutsDetected"] = static_cast<qint64>(stats_.sceneCutsDetected);
    m["motionConfidence"] = stats_.motionConfidence;
    m["reactiveAverage"] = stats_.reactiveAverage;
    m["vramEstimate"] = static_cast<qint64>(stats_.vramEstimate);
    return m;
}

} // namespace temporal_forge
