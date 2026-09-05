// VideoSurfaceItem.cpp — Qt Quick scene-graph adapter for Vulkan images.
//
// Upstream: QML calls refresh(), while PlaybackEngine publishes the current
// raw and reconstructed VkImage handles. Downstream: Qt's render thread wraps
// those handles as QSG textures and draws the aspect/zoom/compare geometry.
// This file must not perform decode or neural work; its performance-sensitive
// job is presentation and its lifetime safety comes from PlaybackEngine's
// queue-idle teardown contract.
#include "ui/VideoSurfaceItem.hpp"

#include <QQuickWindow>
#include <QQmlContext>
#include <QSGSimpleTextureNode>
#include <QSGGeometry>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <algorithm>

namespace temporal_forge {

// Constructor: opt the item into scene-graph content so Qt invokes
// updatePaintNode on the render thread after refresh() schedules an update.
VideoSurfaceItem::VideoSurfaceItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

// Destructor: release only the QSG texture wrappers owned by this item. The
// underlying VkImages remain owned by PlaybackEngine/GpuImageUploader.
VideoSurfaceItem::~VideoSurfaceItem() {
    delete m_nativeTexture;
    delete m_rawTexture;
}

// setPresentationScaler: choose the presentation-only filter and request a
// scene-graph refresh. It must not recreate the FSR target; source/preset
// scaling belongs to PlaybackEngine and the backend selector.
void VideoSurfaceItem::setPresentationScaler(int v) {
    v = std::max(0, std::min(4, v));
    if (v == m_presentationScaler) return;
    m_presentationScaler = v;
    if (m_nativeTexture)
        m_nativeTexture->setFiltering(QSGTexture::Linear);
    update();
}

// refresh: advance to the next displayable frame and schedule a repaint.
//
// Called by: the QML Timer in Main.qml (~source PTS cadence, ~60Hz cap).
// Calls:     PlaybackEngine::advanceRenderFrame (advances the frame queue),
//            then update() to trigger a scene-graph sync.
// Notes:     Frame pacing stays owned by PlaybackEngine — the shared Vulkan
//            image holds the pixels; this only advances the queue + requests
//            an update. Resolves the PlaybackEngine from the QML context lazily.
void VideoSurfaceItem::refresh() {
    if (!m_engine) {
        auto* ctx = qmlContext(this);
        if (ctx) m_engine = qobject_cast<PlaybackEngine*>(ctx->contextProperty("playback").value<QObject*>());
    }
    if (!m_engine) return;

    // Frame pacing remains owned by PlaybackEngine. The pixels stay in the
    // shared Vulkan image; this call only advances the queue and schedules a
    // scene-graph update.
    if (!m_engine->advanceRenderFrame()) return;
    ++m_frameCounter;
    emit frameChanged();
    update();
}

// updatePaintNode: build/update the scene-graph node that samples the FSR4
//                   output image directly from the shared Vulkan image.
//
// Called by: the Qt render thread (scene graph) at ~60Hz whenever update()
//            was called or the item geometry changed.
// Calls:     PlaybackEngine::fsr4NativeOutput / fsr4RawOutput (THE load-bearing
//            render-thread accessors — they deliberately do NOT take the
//            dispatch mutex; teardown safety comes from vkQueueWaitIdle),
//            QSGVulkanTexture::fromNative to wrap the VkImage for the scene graph.
// Notes:     CRITICAL INVARIANT (regression lesson 2026-07-21): the fsr4*Output
//            accessors must never lock fsrDispatchMutex_. Locking them serialized
//            every 60Hz render frame against every 5ms FSR4 dispatch and caused
//            visible stutter. Handles aspect-fit/fill/stretch + zoom/pan +
//            split-screen compare (raw vs enhanced) and raw-only passthrough.
//            Caches the QSGTexture until the underlying VkImage/dims change.
QSGNode* VideoSurfaceItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!m_engine) {
        auto* ctx = qmlContext(this);
        if (ctx)
            m_engine = qobject_cast<PlaybackEngine*>(
                ctx->contextProperty("playback").value<QObject*>());
    }
    if (!m_engine || !window()) return node;

    VkImage fsrImage = VK_NULL_HANDLE, rawImage = VK_NULL_HANDLE;
    uint32_t fsrW = 0, fsrH = 0, rawW = 0, rawH = 0;
    const bool haveFsr = m_engine->fsr4NativeOutput(fsrImage, fsrW, fsrH);
    const bool haveRaw = m_engine->fsr4RawOutput(rawImage, rawW, rawH);
    if (!haveFsr && !m_rawOnly) return oldNode;

    const QSize fsrSize{int(fsrW), int(fsrH)};
    if (haveFsr && (!m_nativeTexture || m_nativeImage != fsrImage || m_nativeSize != fsrSize)) {
        delete m_nativeTexture;
        m_nativeTexture = QNativeInterface::QSGVulkanTexture::fromNative(
            fsrImage, VK_IMAGE_LAYOUT_GENERAL, window(), fsrSize);
        if (!m_nativeTexture) return oldNode;
        // Video presentation should use reconstruction-friendly linear
        // filtering by default. Nearest filtering exposes hard source-pixel
        // blocks when low-resolution video is enlarged.
        m_nativeTexture->setFiltering(QSGTexture::Linear);
        m_nativeImage = fsrImage;
        m_nativeSize = fsrSize;
    }
    if ((m_compareActive || m_rawOnly) && haveRaw) {
        const QSize rawSize{int(rawW), int(rawH)};
        if (!m_rawTexture || m_rawImage != rawImage || m_rawSize != rawSize) {
            delete m_rawTexture;
            m_rawTexture = QNativeInterface::QSGVulkanTexture::fromNative(
                rawImage, VK_IMAGE_LAYOUT_GENERAL, window(), rawSize);
            if (!m_rawTexture) return oldNode;
            m_rawTexture->setFiltering(QSGTexture::Linear);
            m_rawImage = rawImage;
            m_rawSize = rawSize;
        }
    }

    const QRectF bounds = boundingRect();
    const bool showRawOnly = m_rawOnly && m_rawTexture;
    if (!showRawOnly && (!haveFsr || !m_nativeTexture)) return oldNode;
    // Compare panes must preserve the source image aspect ratio.  The raw
    // compare surface is intentionally rawOnly, so it used to return the
    // full item bounds here and stretch the left half to the window aspect.
    // Use the actual texture dimensions for that path as well.
    const qreal srcAspect = showRawOnly
                                ? qreal(rawW) / std::max(1u, rawH)
                                : qreal(fsrW) / std::max(1u, fsrH);
    qreal targetAspect = srcAspect;
    switch (m_aspectMode) {
        case Aspect_16_9: targetAspect = 16.0 / 9.0; break;
        case Aspect_4_3: targetAspect = 4.0 / 3.0; break;
        case Aspect_235_1: targetAspect = 2.35; break;
        case AspectStretch: targetAspect = bounds.width() / std::max(qreal(1), bounds.height()); break;
        default: break;
    }
    qreal drawnW = bounds.width(), drawnH = bounds.height();
    const qreal boundsAspect = bounds.width() / std::max(qreal(1), bounds.height());
    if (m_aspectMode == AspectFit || m_aspectMode >= Aspect_16_9) {
        if (targetAspect > boundsAspect) drawnH = bounds.width() / targetAspect;
        else drawnW = bounds.height() * targetAspect;
    } else if (m_aspectMode == AspectFill) {
        if (targetAspect > boundsAspect) drawnW = bounds.height() * targetAspect;
        else drawnH = bounds.width() / targetAspect;
    }
    drawnW *= m_zoomFactor; drawnH *= m_zoomFactor;
    const qreal cx = bounds.center().x() + m_panX * bounds.width() * 0.5;
    const qreal cy = bounds.center().y() + m_panY * bounds.height() * 0.5;
    const QRectF imageRect(cx - drawnW * 0.5, cy - drawnH * 0.5, drawnW, drawnH);

    if (showRawOnly) {
        if (!node) node = new QSGSimpleTextureNode;
        node->setTexture(m_rawTexture);
        node->setRect(imageRect);
        return node;
    }
    if (m_compareActive && m_rawTexture) {
        auto* root = oldNode;
        if (!root || !root->firstChild()) {
            delete oldNode;
            root = new QSGNode;
            auto* left = new QSGSimpleTextureNode;
            auto* right = new QSGSimpleTextureNode;
            root->appendChildNode(left); root->appendChildNode(right);
        }
        auto* left = static_cast<QSGSimpleTextureNode*>(root->firstChild());
        auto* right = static_cast<QSGSimpleTextureNode*>(root->lastChild());
        const qreal split = bounds.left() + bounds.width() * m_compareSplit;
        left->setTexture(m_compareRawOnLeft ? m_rawTexture : m_nativeTexture);
        right->setTexture(m_compareRawOnLeft ? m_nativeTexture : m_rawTexture);
        left->setRect(imageRect.intersected(QRectF(bounds.left(), bounds.top(), split - bounds.left(), bounds.height())));
        right->setRect(imageRect.intersected(QRectF(split, bounds.top(), bounds.right() - split, bounds.height())));
        return root;
    }
    if (!node) node = new QSGSimpleTextureNode;
    node->setTexture(m_nativeTexture);
    node->setRect(imageRect);
    return node;
}

} // namespace temporal_forge
