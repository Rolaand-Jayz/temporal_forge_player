// VideoSurfaceItem.hpp — native Vulkan QQuickItem that samples the completed
// FSR4 output image directly. The raw compare instance selects the uploader's
// decoded-frame presentation image.
//
// Aspect ratio, zoom, and pan are handled here in the geometry pass (real, not
// stubs — they actually crop / scale the displayed rect). Post-processing
// filters are applied in QML via a ShaderEffect wrapping this item, so this
// class deliberately keeps the simple QSGSimpleTextureNode path that has been
// validated not to regress playback.
#pragma once
#include "../core/PlaybackEngine.hpp"

#include <QQuickItem>
#include <QSGNode>
#include <QSGTexture>

#include <vulkan/vulkan.h>

namespace temporal_forge {

class VideoSurfaceItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int frameCounter READ frameCounter NOTIFY frameChanged)
    // Color settings (bound from QML to videoSettings properties).
    Q_PROPERTY(float brightness READ brightness WRITE setBrightness)
    Q_PROPERTY(float contrast READ contrast WRITE setContrast)
    Q_PROPERTY(float saturation READ saturation WRITE setSaturation)
    Q_PROPERTY(float hue READ hue WRITE setHue)
    Q_PROPERTY(float gamma READ gamma WRITE setGamma)
    Q_PROPERTY(int presentationScaler READ presentationScaler WRITE setPresentationScaler)
    // Compare mode (bound from QML to compare properties).
    Q_PROPERTY(bool compareActive READ compareActive WRITE setCompareActive)
    Q_PROPERTY(float compareSplit READ compareSplit WRITE setCompareSplit)
    Q_PROPERTY(bool compareRawOnLeft READ compareRawOnLeft WRITE setCompareRawOnLeft)
    Q_PROPERTY(bool rawOnly READ rawOnly WRITE setRawOnly)
    // Aspect ratio + zoom / pan (driven from the right-click context menu).
    Q_PROPERTY(int aspectMode READ aspectMode WRITE setAspectMode)
    Q_PROPERTY(float zoomFactor READ zoomFactor WRITE setZoomFactor)
    Q_PROPERTY(float panX READ panX WRITE setPanX)
    Q_PROPERTY(float panY READ panY WRITE setPanY)

public:
    enum AspectMode : int {
        AspectFit = 0,    // letterbox to fit inside bounds (no crop)
        AspectFill = 1,   // fill bounds, crop overflowing edges
        Aspect_16_9 = 2,  // force 16:9 inside bounds
        Aspect_4_3 = 3,   // force 4:3 inside bounds
        Aspect_235_1 = 4, // force 2.35:1 (cinema) inside bounds
        AspectStretch = 5 // stretch to bounds (ignore aspect ratio)
    };
    Q_ENUM(AspectMode)

    VideoSurfaceItem(QQuickItem* parent = nullptr);
    ~VideoSurfaceItem();

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void requestUpdate() { update(); }

    int frameCounter() const { return m_frameCounter; }

    // Color accessors (0 = neutral, gamma 1.0 = neutral).
    float brightness() const { return m_brightness; }
    float contrast() const { return m_contrast; }
    float saturation() const { return m_saturation; }
    float hue() const { return m_hue; }
    float gamma() const { return m_gamma; }
    int presentationScaler() const { return m_presentationScaler; }
    void setBrightness(float v) { m_brightness = v; }
    void setContrast(float v) { m_contrast = v; }
    void setSaturation(float v) { m_saturation = v; }
    void setHue(float v) { m_hue = v; }
    void setGamma(float v) { m_gamma = v; }
    void setPresentationScaler(int v);

    bool compareActive() const { return m_compareActive; }
    void setCompareActive(bool a) { if (a != m_compareActive) { m_compareActive = a; update(); } }
    float compareSplit() const { return m_compareSplit; }
    void setCompareSplit(float s) { s = std::clamp(s, 0.05f, 0.95f); if (s != m_compareSplit) { m_compareSplit = s; update(); } }
    bool compareRawOnLeft() const { return m_compareRawOnLeft; }
    void setCompareRawOnLeft(bool r) { if (r != m_compareRawOnLeft) { m_compareRawOnLeft = r; update(); } }
    bool rawOnly() const { return m_rawOnly; }
    void setRawOnly(bool v) { if (v != m_rawOnly) { m_rawOnly = v; update(); } }

    int aspectMode() const { return m_aspectMode; }
    void setAspectMode(int v) { if (v != m_aspectMode) { m_aspectMode = v; update(); } }
    float zoomFactor() const { return m_zoomFactor; }
    void setZoomFactor(float v) {
        v = std::clamp(v, 1.0f, 8.0f);
        if (v != m_zoomFactor) { m_zoomFactor = v; update(); }
    }
    float panX() const { return m_panX; }
    void setPanX(float v) {
        v = std::clamp(v, -1.0f, 1.0f);
        if (v != m_panX) { m_panX = v; update(); }
    }
    float panY() const { return m_panY; }
    void setPanY(float v) {
        v = std::clamp(v, -1.0f, 1.0f);
        if (v != m_panY) { m_panY = v; update(); }
    }

signals:
    void frameChanged();

private:
    PlaybackEngine* m_engine = nullptr;
    QSGTexture* m_nativeTexture = nullptr;
    QSGTexture* m_rawTexture = nullptr;
    VkImage m_nativeImage = VK_NULL_HANDLE;
    VkImage m_rawImage = VK_NULL_HANDLE;
    QSize m_nativeSize;
    QSize m_rawSize;
    int m_frameCounter = 0;

    // Color (applied in QML ShaderEffect post-process pass).
    float m_brightness = 0.0f;
    float m_contrast = 0.0f;
    float m_saturation = 0.0f;
    float m_hue = 0.0f;
    float m_gamma = 1.0f;
    int m_presentationScaler = 0;

    // Compare mode.
    bool m_compareActive = false;
    float m_compareSplit = 0.5f;
    bool m_compareRawOnLeft = true;
    bool m_rawOnly = false;

    // Aspect / zoom / pan.
    int m_aspectMode = AspectFit;
    float m_zoomFactor = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;
};

} // namespace temporal_forge
