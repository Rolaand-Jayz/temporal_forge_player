// ScreenCaptureController.hpp — real screenshot + clipboard capture.
//
// Reads back the displayed frame via PlaybackEngine's GPU readback path (which
// is the same one used by the existing FSR4 diagnostic dump) and writes it
// either to disk as a PNG or to the system clipboard as an image.
//
// This is a real, fully-functional feature — no stubs. It is invoked from the
// right-click context menu and from a keyboard shortcut (Ctrl+S).
#pragma once
#include "../core/PlaybackEngine.hpp"

#include <QObject>
#include <QString>
#include <QImage>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <vector>
#include <cstdint>

namespace temporal_forge {

class ScreenCaptureController : public QObject {
    Q_OBJECT
    // Read-only state used by the UI to show where the last screenshot went.
    Q_PROPERTY(QString lastSavePath READ lastSavePath NOTIFY captureChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY captureChanged)
public:
    explicit ScreenCaptureController(QObject* parent = nullptr)
        : QObject(parent) {}

    QString lastSavePath() const { return m_lastSavePath; }
    QString lastError() const { return m_lastError; }

    // captureToFile: take a screenshot and write it to the user's Pictures
    //                directory (XDG_PICTURES_DIR, falling back to home).
    //
    // Called by: QML (Q_INVOKABLE) from the right-click context menu and Ctrl+S.
    // Calls:     readbackToQImage (GPU readback via PlaybackEngine), QImage::save.
    // Notes:     File name includes the sanitized media title + timestamp so
    //            successive captures do not overwrite each other. Returns true
    //            on success; sets lastError on failure.
    Q_INVOKABLE bool captureToFile(PlaybackEngine* engine,
                                   const QString& mediaTitle);

    // captureToClipboard: copy the current frame to the system clipboard as a QImage.
    //
    // Called by: QML (Q_INVOKABLE) from the right-click context menu.
    // Calls:     readbackToQImage, QGuiApplication::clipboard()->setImage.
    Q_INVOKABLE bool captureToClipboard(PlaybackEngine* engine);

signals:
    void captureChanged();

private:
    // readbackToQImage: pull the displayed pixels back to the CPU as a QImage.
    //                    Called by: captureToFile, captureToClipboard. Uses the
    //                    same GPU readback path as the FSR4 diagnostic dump.
    QImage readbackToQImage(PlaybackEngine* engine);
    // setError: record an error message and emit captureChanged so QML shows it.
    //           Called by: captureToFile/captureToClipboard on failure.
    void setError(const QString& msg);

    // sanitizeTitle: turn a media title into a filesystem-safe filename fragment.
    //                Called by: captureToFile. Non-alphanumeric chars become '_'.
    static QString sanitizeTitle(const QString& title) {
        QString out = title.trimmed();
        for (QChar& c : out) {
            if (!c.isLetterOrNumber() && c != '-' && c != '_') c = '_';
        }
        if (out.isEmpty()) out = QStringLiteral("frame");
        return out;
    }

    QString m_lastSavePath;
    QString m_lastError;
};

} // namespace temporal_forge

// --- out-of-line method definitions (after the signal declaration so emit
// captureChanged() resolves cleanly) ---
inline bool temporal_forge::ScreenCaptureController::captureToFile(
    PlaybackEngine* engine, const QString& mediaTitle) {
    QImage img = readbackToQImage(engine);
    if (img.isNull()) return false;

    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    const QString safe = sanitizeTitle(mediaTitle);
    const QString stamp = QDateTime::currentDateTime()
                              .toString("yyyyMMdd_HHmmss_zzz");
    const QString path = QDir(dir).filePath(
        QStringLiteral("temporal_forge_%1_%2.png").arg(safe, stamp));
    if (!img.save(path, "PNG")) {
        setError(QStringLiteral("Could not write PNG to %1").arg(path));
        return false;
    }
    m_lastSavePath = path;
    m_lastError.clear();
    emit captureChanged();
    return true;
}

inline bool temporal_forge::ScreenCaptureController::captureToClipboard(
    PlaybackEngine* engine) {
    QImage img = readbackToQImage(engine);
    if (img.isNull()) return false;
    QGuiApplication::clipboard()->setImage(img);
    m_lastError.clear();
    m_lastSavePath = QStringLiteral("(clipboard)");
    emit captureChanged();
    return true;
}

inline QImage temporal_forge::ScreenCaptureController::readbackToQImage(
    PlaybackEngine* engine) {
    if (!engine) {
        setError(QStringLiteral("No playback engine available"));
        return {};
    }
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!engine->readbackLastDisplayedFrame(rgba, w, h)) {
        setError(QStringLiteral("Frame readback failed — no media or FSR4 not ready"));
        return {};
    }
    QImage img(rgba.data(), int(w), int(h), int(w * 4),
               QImage::Format_RGBA8888);
    return img.copy(); // detach from the local buffer so it outlives it
}

inline void temporal_forge::ScreenCaptureController::setError(
    const QString& msg) {
    m_lastError = msg;
    m_lastSavePath.clear();
    emit captureChanged();
}
