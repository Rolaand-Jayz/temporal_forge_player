// VideoSettingsController.hpp — QML bridge for video display settings.
//
// Exposes color adjustments (brightness/contrast/saturation/hue/gamma) and
// video tuning controls to QML. Changes emit colorChanged()/tuningChanged()
// so the render path can re-apply them at the next frame. All values are
// normalized:
//   brightness/contrast/saturation/hue: [-1.0, 1.0] (0 = neutral)
//   gamma: [0.1, 3.0] (1.0 = neutral)
//   sharpness: [0.0, 1.0] (0 = off)
//   jitterStrength: [0.2, 1.5] (1.0 = default)
//
// The controller does NOT own the Settings struct — it reads/writes the fields
// on a Settings reference owned by main.cpp, so persistence flows through the
// existing SettingsStore.
#pragma once
#include "config/SettingsStore.hpp"

#include <QObject>

namespace temporal_forge {

class VideoSettingsController : public QObject {
    Q_OBJECT
    Q_PROPERTY(float brightness READ brightness WRITE setBrightness NOTIFY colorChanged)
    Q_PROPERTY(float contrast READ contrast WRITE setContrast NOTIFY colorChanged)
    Q_PROPERTY(float saturation READ saturation WRITE setSaturation NOTIFY colorChanged)
    Q_PROPERTY(float hue READ hue WRITE setHue NOTIFY colorChanged)
    Q_PROPERTY(float gamma READ gamma WRITE setGamma NOTIFY colorChanged)
    Q_PROPERTY(float sharpness READ sharpness WRITE setSharpness NOTIFY tuningChanged)
    Q_PROPERTY(float jitterStrength READ jitterStrength WRITE setJitterStrength NOTIFY tuningChanged)
    Q_PROPERTY(int presentationScaler READ presentationScaler WRITE setPresentationScaler NOTIFY tuningChanged)

public:
    explicit VideoSettingsController(QObject* parent = nullptr);

    // Wire to the Settings struct owned by main.cpp.
    void setSettings(Settings* s) { settings_ = s; }

    float brightness() const  { return settings_ ? settings_->brightness  : 0.0f; }
    float contrast() const    { return settings_ ? settings_->contrast    : 0.0f; }
    float saturation() const  { return settings_ ? settings_->saturation  : 0.0f; }
    float hue() const         { return settings_ ? settings_->hue         : 0.0f; }
    float gamma() const       { return settings_ ? settings_->gamma       : 1.0f; }
    float sharpness() const    { return settings_ ? settings_->sharpness    : 0.3f; }
    float jitterStrength() const { return settings_ ? settings_->jitterStrength : 1.0f; }
    int presentationScaler() const {
        return settings_ ? static_cast<int>(settings_->presentationScaler) :
                           static_cast<int>(PresentationScaler::Bicubic);
    }

    void setBrightness(float v);
    void setContrast(float v);
    void setSaturation(float v);
    void setHue(float v);
    void setGamma(float v);
    void setSharpness(float v);
    void setJitterStrength(float v);
    void setPresentationScaler(int v);

    // Reset all color adjustments to neutral.
    Q_INVOKABLE void resetColor();
    Q_INVOKABLE void resetTuning();

signals:
    void colorChanged();
    void tuningChanged();

private:
    Settings* settings_ = nullptr;
};

} // namespace temporal_forge
