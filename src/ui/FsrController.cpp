// FsrController.cpp
#include "ui/FsrController.hpp"
#include "core/PlaybackEngine.hpp"
#include "util/FsrTargetMath.hpp"
#include <algorithm>
#include <cmath>

namespace temporal_forge {

FsrController::FsrController(PlaybackEngine* engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    // Poll the engine for source dimensions. Cheap; tied to media changes.
    refreshSource();
}

void FsrController::refreshSource() {
    int w = 0, h = 0;
    if (engine_) engine_->sourceDimensions(w, h);
    if (w != srcW_ || h != srcH_) {
        srcW_ = w; srcH_ = h;
        emit sourceChanged();
        emit scalingChanged();
    }
}

int FsrController::fsrTargetWidth() const {
    if (srcW_ == 0) return 0;
    const Size2D t = fsrTargetSize(srcW_, srcH_, preset_, 2);
    return static_cast<int>(t.width);
}

int FsrController::fsrTargetHeight() const {
    if (srcH_ == 0) return 0;
    const Size2D t = fsrTargetSize(srcW_, srcH_, preset_, 2);
    return static_cast<int>(t.height);
}

QString FsrController::presetLabel() const {
    if (srcW_ == 0) return QString("FSR: ") + presetDisplayName(preset_);
    const Size2D t = fsrTargetSize(srcW_, srcH_, preset_, 2);
    return QString("FSR: %1 (%2x%3 -> %4x%5)")
        .arg(presetDisplayName(preset_))
        .arg(srcW_).arg(srcH_)
        .arg(t.width).arg(t.height);
}

QString FsrController::backendLabel() const {
    return QString::fromUtf8(backendDisplayName(backend_));
}

// spec 05 "UX Principle": always show source -> FSR target -> window.
QString FsrController::chainLabel() const {
    if (srcW_ == 0) return QStringLiteral("No media loaded");
    Size2D t = fsrTargetSize(srcW_, srcH_, preset_, 2);
    if (backend_ == BackendKind::Fsr4ReExperimental &&
        preset_ == UpscalePreset::UltraPerformance) {
        const Size2D nativeTarget = nativeInt8UltraPerformanceTarget(srcW_, srcH_);
        if (nativeTarget.width != 0) t = nativeTarget;
    }
    return QString("%1x%2 -> FSR %3 %4x%5 -> Window %6x%7")
        .arg(srcW_).arg(srcH_)
        .arg(presetDisplayName(preset_))
        .arg(t.width).arg(t.height)
        .arg(winW_).arg(winH_);
}

// setPreset: apply a new FSR preset, driving the engine's enable + viewport.
//
// Called by: QML preset selector (Q_PROPERTY write).
// Calls:     presetRatio, engine_->setFsrViewport / setFsr4Enabled.
// Notes:     The Off<->any and ratio-equal-but-preset-differs cases force a
//            teardown via a tiny viewport-scale perturbation because
//            setFsrViewport is a no-op when the ratio is unchanged (regression
//            lesson 2026-07-20: leaving the old dispatch running after toggling
//            Off). setFsr4Enabled(preset != Off && experimental) tears the
//            harness down immediately on Off.
void FsrController::setPreset(UpscalePreset p) {
    // The controller starts with a UI-side default, while PlaybackEngine has
    // its own scale cache.  The first synchronization must therefore reach
    // the engine even when the selected preset equals preset_.  Otherwise a
    // saved Quality/NativeAA selection can silently leave the engine at its
    // constructor scale (historically Performance/2.0x).
    const bool samePreset = p == preset_;
    const UpscalePreset oldPreset = preset_;
    preset_ = p;
    if (engine_) {
        // presetRatio() returns 1.0 for both Off and NativeAA, which makes
        // setFsrViewport() a no-op (the cached viewport is unchanged). For
        // a graceful mode switch we need to actually retire the old dispatch
        // path, so force a rebuild by overriding the viewport scale when the
        // ratio is identical but the preset differs.
        const float newRatio = presetRatio(preset_);
        const float oldRatio = presetRatio(oldPreset);
        if (newRatio != oldRatio || samePreset) {
            engine_->setFsrViewport(static_cast<uint32_t>(std::max(2, winW_)),
                                    static_cast<uint32_t>(std::max(2, winH_)),
                                    newRatio);
        } else if (preset_ == UpscalePreset::Off ||
                   oldPreset == UpscalePreset::Off) {
            // Same numeric ratio but the user explicitly entered or left
            // the Off state. Force a teardown by poking the viewport scale
            // through a tiny perturbation so setFsrViewport actually fires.
            engine_->setFsrViewport(static_cast<uint32_t>(std::max(2, winW_)),
                                    static_cast<uint32_t>(std::max(2, winH_)),
                                    std::max(1.0f, newRatio + 0.001f));
            engine_->setFsrViewport(static_cast<uint32_t>(std::max(2, winW_)),
                                    static_cast<uint32_t>(std::max(2, winH_)),
                                    newRatio);
        }
        // Drive the actual enable/disable from the preset so toggling Off
        // tears the harness down immediately (no more "FSR: Off" chain
        // shown while a dispatch is still running).
        engine_->setFsr4Enabled(preset_ != UpscalePreset::Off &&
                                 backend_ == BackendKind::Fsr4ReExperimental);
    }
    if (!samePreset) {
        emit presetChanged();
        emit scalingChanged();
    }
}

// setBackend: apply a new backend kind, aligning the live FSR4 enable state.
//
// Called by: QML backend selector (Q_PROPERTY write).
// Calls:     engine_->setFsr4Enabled.
// Notes:     Only the RE-derived INT8 backend is wired to the live Vulkan
//            dispatch; any other selection disables upscaling so playback falls
//            through to the raw decoded-frame path cleanly. Even a no-op
//            reselection re-aligns enable with the chosen backend (fixes the
//            regression where toggling the same backend twice re-enabled FSR4).
void FsrController::setBackend(BackendKind b) {
    if (b == backend_) {
        // Even on a no-op selection we keep the FSR4 enable state aligned
        // with the chosen backend so reselecting the experimental backend
        // toggles the real path back on after a preset-driven disable.
        if (engine_)
            engine_->setFsr4Enabled(b == BackendKind::Fsr4ReExperimental &&
                                    preset_ != UpscalePreset::Off);
        return;
    }
    // spec 04 / 05: Auto resolves to FSR 2.3 SDK when available. We don't
    // expose Auto as a separate enum here for MVP; "Auto" maps to Fsr23Sdk.
    if (b == BackendKind::Fsr4ReExperimental) {
        // Experimental is selectable and intentionally exposed for RDNA3/Linux
        // validation. A UI warning is shown on selection.
    }
    backend_ = b;
    if (engine_) {
        // Only the RE-derived INT8 backend is wired to the live Vulkan
        // dispatch in this build. Any other selection disables upscaling
        // so the player falls through to the raw decoded frame path
        // cleanly instead of crashing mid-dispatch.
        engine_->setFsr4Enabled(b == BackendKind::Fsr4ReExperimental &&
                                preset_ != UpscalePreset::Off);
    }
    emit backendChanged();
    emit scalingChanged();
}

// setWindowSize: track window resize from QML; forwards to the engine so the
//                presentation scale updates. NOTE: per spec 02 and the resize
//                regression lesson, this must NOT recreate the FSR context —
//                setFsrViewport only affects presentation, never the FSR target.
//                Called by: QML on window resize (Q_INVOKABLE).
void FsrController::setWindowSize(int w, int h) {
    if (w == winW_ && h == winH_) return;
    winW_ = w; winH_ = h;
    if (engine_)
        engine_->setFsrViewport(static_cast<uint32_t>(std::max(2, w)),
                                static_cast<uint32_t>(std::max(2, h)),
                                presetRatio(preset_));
    emit scalingChanged();
}

QVariantList FsrController::presetOptions() const {
    QVariantList out;
    for (size_t i = 0; i < kPresetTableSize; ++i) {
        const auto& p = kPresetTable[i];
        QVariantMap m;
        m["key"] = static_cast<int>(p.preset);
        m["label"] = QString::fromUtf8(p.displayName);
        m["ratio"] = p.ratio;
        m["temporal"] = p.temporal;
        if (srcW_ > 0) {
            Size2D t = fsrTargetSize(srcW_, srcH_, p.preset, 2);
            if (backend_ == BackendKind::Fsr4ReExperimental &&
                p.preset == UpscalePreset::UltraPerformance) {
                const Size2D nativeTarget = nativeInt8UltraPerformanceTarget(srcW_, srcH_);
                if (nativeTarget.width != 0) t = nativeTarget;
            }
            m["targetW"] = static_cast<int>(t.width);
            m["targetH"] = static_cast<int>(t.height);
        }
        out.append(m);
    }
    return out;
}

QVariantList FsrController::backendOptions() const {
    QVariantList out;
    // v3→10 policy: FSR4 INT8 experimental default on RDNA3, FSR 3.1.5
    // fallback, spatial. Correct wording only — never "official" for INT8.
    auto add = [&](int key, const char* name, bool experimental) {
        QVariantMap m;
        m["key"] = key;
        m["label"] = QString::fromUtf8(name);
        m["experimental"] = experimental;
        out.append(m);
    };
    add(static_cast<int>(BackendKind::Fsr4ReExperimental),
        "FSR 4.1 INT8 — RDNA3, RE-derived, experimental", true);
    add(static_cast<int>(BackendKind::Fsr23Sdk),
        "FSR 3.1.5 (fallback)", false);
    add(static_cast<int>(BackendKind::SpatialFallback),
        "Spatial fallback", false);
    return out;
}

} // namespace temporal_forge
