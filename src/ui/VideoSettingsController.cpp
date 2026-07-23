// VideoSettingsController.cpp
#include "ui/VideoSettingsController.hpp"

#include <algorithm>

namespace temporal_forge {

VideoSettingsController::VideoSettingsController(QObject* parent)
    : QObject(parent) {}

// clampf: file-local clamp helper bounding v to [lo, hi].
//         Called by: the set* methods below.
static float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

// --- Color setters ---
// All color setters (setBrightness/Contrast/Saturation/Hue in [-1,1], setGamma
// in [0.1,3.0]) follow the same pattern:
//   Called by: QML (Q_PROPERTY writes from the color sliders).
//   Calls:     clampf, then emit colorChanged() if the clamped value changed.
//   Notes:     Operate on the Settings struct owned by main.cpp; the render path
//              re-reads it next frame. No-op when settings_ is null (not wired yet).
void VideoSettingsController::setBrightness(float v) {
    if (!settings_) return;
    v = clampf(v, -1.0f, 1.0f);
    if (v == settings_->brightness) return;
    settings_->brightness = v;
    emit colorChanged();
}

void VideoSettingsController::setContrast(float v) {
    if (!settings_) return;
    v = clampf(v, -1.0f, 1.0f);
    if (v == settings_->contrast) return;
    settings_->contrast = v;
    emit colorChanged();
}

void VideoSettingsController::setSaturation(float v) {
    if (!settings_) return;
    v = clampf(v, -1.0f, 1.0f);
    if (v == settings_->saturation) return;
    settings_->saturation = v;
    emit colorChanged();
}

void VideoSettingsController::setHue(float v) {
    if (!settings_) return;
    v = clampf(v, -1.0f, 1.0f);
    if (v == settings_->hue) return;
    settings_->hue = v;
    emit colorChanged();
}

void VideoSettingsController::setGamma(float v) {
    if (!settings_) return;
    v = clampf(v, 0.1f, 3.0f);
    if (v == settings_->gamma) return;
    settings_->gamma = v;
    emit colorChanged();
}

// --- Tuning setters ---
// setSharpness [0,1] and setJitterStrength [0.2,1.5] clamp + emit tuningChanged
// (which the FSR4 dispatch reads to re-apply sharpness/jitter next frame).
// Called by QML. setPresentationScaler maps an int [0,4] to PresentationScaler.
void VideoSettingsController::setSharpness(float v) {
    if (!settings_) return;
    v = clampf(v, 0.0f, 1.0f);
    if (v == settings_->sharpness) return;
    settings_->sharpness = v;
    emit tuningChanged();
}

void VideoSettingsController::setJitterStrength(float v) {
    if (!settings_) return;
    v = clampf(v, 0.2f, 1.5f);
    if (v == settings_->jitterStrength) return;
    settings_->jitterStrength = v;
    emit tuningChanged();
}

void VideoSettingsController::setPresentationScaler(int v) {
    if (!settings_) return;
    v = std::max(0, std::min(4, v));
    auto scaler = static_cast<PresentationScaler>(v);
    if (scaler == settings_->presentationScaler) return;
    settings_->presentationScaler = scaler;
    emit tuningChanged();
}

// resetColor: restore brightness/contrast/saturation/hue/gamma to neutral.
//             Called by: QML "reset color" button (Q_INVOKABLE).
void VideoSettingsController::resetColor() {
    if (!settings_) return;
    settings_->brightness = 0.0f;
    settings_->contrast = 0.0f;
    settings_->saturation = 0.0f;
    settings_->hue = 0.0f;
    settings_->gamma = 1.0f;
    emit colorChanged();
}

// resetTuning: restore sharpness/jitterStrength to defaults.
//              Called by: QML "reset tuning" button (Q_INVOKABLE).
void VideoSettingsController::resetTuning() {
    if (!settings_) return;
    settings_->sharpness = 0.3f;
    settings_->jitterStrength = 1.0f;
    emit tuningChanged();
}

} // namespace temporal_forge
