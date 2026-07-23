// PostProcessController.hpp — QML bridge for the post-processing filter chain.
//
// Real values consumed by VideoFilterNode's fragment shader each frame. Every
// field here is wired to a real GPU effect in the display pass; nothing is a
// stub. A field set to its neutral value is equivalent to disabling the
// effect, so individual enable toggles are derived from non-zero / non-default
// values rather than tracked separately (keeps the shader and persistence
// story honest and simple).
//
// Filter chain order (fixed in the shader, applied left-to-right):
//   1. Display sharpening (unsharp mask)   — displaySharpen
//   2. Deband (ordered dithering)          — debandEnabled
//   3. Bloom (bright-pass + blur + add)    — bloomStrength
//   4. Film grain                          — grainStrength
//
// Letterbox detect/crop is a separate concern: it adjusts the source rectangle
// sampled by the shader (auto-detected each frame by luma row analysis).
#pragma once
#include <QObject>
#include <QRectF>

namespace temporal_forge {

class PostProcessController : public QObject {
    Q_OBJECT

    // Display sharpening (unsharp mask kernel in the final pass).
    // Range [0, 2]. 0 = off, 0.5 = subtle, 1.0 = strong.
    Q_PROPERTY(float displaySharpen READ displaySharpen WRITE setDisplaySharpen NOTIFY chainChanged)

    // Deband: ordered Bayer dithering that breaks up gradient banding in dark
    // scenes. Toggle — when true a small per-pixel dither is added.
    Q_PROPERTY(bool debandEnabled READ debandEnabled WRITE setDebandEnabled NOTIFY chainChanged)

    // Bloom strength [0, 1]. 0 = off. Bright pixels (luma > threshold) bleed
    // into neighbors. Combined with threshold below.
    Q_PROPERTY(float bloomStrength READ bloomStrength WRITE setBloomStrength NOTIFY chainChanged)
    Q_PROPERTY(float bloomThreshold READ bloomThreshold WRITE setBloomThreshold NOTIFY chainChanged)

    // Film grain strength [0, 1]. 0 = off. Added in perceptual space so it
    // survives both dark and bright regions.
    Q_PROPERTY(float grainStrength READ grainStrength WRITE setGrainStrength NOTIFY chainChanged)

    // Letterbox handling. Auto mode scans each displayed frame for hard black
    // bars and crops the sampling rect to the active picture area. Manual mode
    // lets the user specify a fixed crop fraction.
    Q_PROPERTY(int letterboxMode READ letterboxMode WRITE setLetterboxMode NOTIFY chainChanged)
    Q_PROPERTY(float letterboxManualCrop READ letterboxManualCrop WRITE setLetterboxManualCrop NOTIFY chainChanged)
    // Result of the last auto-detect, exposed for the info panel. Updated by
    // the filter node each frame (write-only from QML's perspective).
    Q_PROPERTY(QRectF detectedLetterbox READ detectedLetterbox NOTIFY letterboxDetected)

public:
    enum LetterboxMode : int {
        LetterboxOff = 0,     // show full frame including any bars
        LetterboxAuto = 1,    // auto-detect and crop black bars
        LetterboxManual = 2,  // apply letterboxManualCrop fraction
    };
    Q_ENUM(LetterboxMode)

    explicit PostProcessController(QObject* parent = nullptr) : QObject(parent) {}

    // --- accessors ---
    float displaySharpen() const { return m_displaySharpen; }
    bool debandEnabled() const { return m_debandEnabled; }
    float bloomStrength() const { return m_bloomStrength; }
    float bloomThreshold() const { return m_bloomThreshold; }
    float grainStrength() const { return m_grainStrength; }
    int letterboxMode() const { return m_letterboxMode; }
    float letterboxManualCrop() const { return m_letterboxManualCrop; }
    QRectF detectedLetterbox() const { return m_detectedLetterbox; }

    void setDisplaySharpen(float v) { clampAssign(v, 0.0f, 2.0f, m_displaySharpen); }
    void setDebandEnabled(bool v) { if (m_debandEnabled != v) { m_debandEnabled = v; emit chainChanged(); } }
    void setBloomStrength(float v) { clampAssign(v, 0.0f, 1.0f, m_bloomStrength); }
    void setBloomThreshold(float v) { clampAssign(v, 0.0f, 1.0f, m_bloomThreshold); }
    void setGrainStrength(float v) { clampAssign(v, 0.0f, 1.0f, m_grainStrength); }
    void setLetterboxMode(int v) {
        if (v < LetterboxOff || v > LetterboxManual) return;
        if (m_letterboxMode != v) { m_letterboxMode = v; emit chainChanged(); }
    }
    void setLetterboxManualCrop(float v) { clampAssign(v, 0.0f, 0.25f, m_letterboxManualCrop); }

    // Called by VideoFilterNode after each frame's auto-detect scan.
    void publishDetectedLetterbox(const QRectF& r) {
        if (r == m_detectedLetterbox) return;
        m_detectedLetterbox = r;
        emit letterboxDetected();
    }

    // True when no filter in the chain is active. The shader uses this to skip
    // the post-processing pass entirely and fall back to direct texture
    // sampling — saves GPU time when nothing is requested.
    bool anyFilterActive() const {
        return m_displaySharpen > 0.001f || m_debandEnabled ||
               m_bloomStrength > 0.001f || m_grainStrength > 0.001f;
    }

    // Reset everything to neutral/off. Wired to a "Reset Filters" button.
    Q_INVOKABLE void resetAll() {
        bool changed = false;
        if (m_displaySharpen != 0.0f) { m_displaySharpen = 0.0f; changed = true; }
        if (m_debandEnabled) { m_debandEnabled = false; changed = true; }
        if (m_bloomStrength != 0.0f) { m_bloomStrength = 0.0f; changed = true; }
        if (m_grainStrength != 0.0f) { m_grainStrength = 0.0f; changed = true; }
        if (m_letterboxMode != LetterboxOff) { m_letterboxMode = LetterboxOff; changed = true; }
        if (m_letterboxManualCrop != 0.0f) { m_letterboxManualCrop = 0.0f; changed = true; }
        if (changed) emit chainChanged();
    }

signals:
    void chainChanged();
    void letterboxDetected();

private:
    template <typename T>
    void clampAssign(T v, T lo, T hi, T& slot) {
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (slot != v) { slot = v; emit chainChanged(); }
    }

    float m_displaySharpen = 0.0f;
    bool  m_debandEnabled = false;
    float m_bloomStrength = 0.0f;
    float m_bloomThreshold = 0.7f;   // luma threshold for bloom bright pass
    float m_grainStrength = 0.0f;
    int   m_letterboxMode = LetterboxOff;
    float m_letterboxManualCrop = 0.0f;
    QRectF m_detectedLetterbox{0, 0, 1, 1};
};

} // namespace temporal_forge
