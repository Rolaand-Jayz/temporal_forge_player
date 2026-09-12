// VideoContextMenu.qml — right-click context menu for the video surface.
//
// All items map to real actions: playback control (already wired through
// PlaybackEngine), aspect ratio / zoom / pan (real properties on
// VideoSurfaceItem), A/B compare (CompareController), screenshot / clipboard
// (ScreenCaptureController), and file open / reload. No stub entries.
import QtQuick
import QtQuick.Controls
import TemporalForge

Menu {
    id: root

    // Required so the menu can call into the engine and surface item.
    property var videoSurface: null
    property var playlistPopup: null

    // Top-level: Play/Pause + Mute.
    MenuItem {
        text: playback.playing ? "❚❚  Pause" : "▶  Play"
        enabled: playback.hasMedia
        onTriggered: playback.togglePlay()
    }
    MenuItem {
        text: playback.muted ? "🔇  Unmute" : "🔊  Mute"
        onTriggered: playback.muted = !playback.muted
    }

    Menu {
        title: "Volume"
        Slider {
            from: 0; to: 100; value: playback.volume
            onMoved: playback.volume = value
            width: parent ? parent.width - 24 : 180
        }
    }

    MenuSeparator {}

    // --- Aspect ratio submenu ---
    // Real: each entry sets videoSurface.aspectMode which the C++ geometry
    // path uses to compute the drawn rect.
    Menu {
        title: "Aspect Ratio"
        ButtonGroup { id: aspectGroup }
        MenuItem {
            text: "Fit (letterbox)"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.AspectFit
            onTriggered: root.videoSurface.aspectMode = VideoSurface.AspectFit
        }
        MenuItem {
            text: "Fill (crop)"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.AspectFill
            onTriggered: root.videoSurface.aspectMode = VideoSurface.AspectFill
        }
        MenuItem {
            text: "16:9"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.Aspect_16_9
            onTriggered: root.videoSurface.aspectMode = VideoSurface.Aspect_16_9
        }
        MenuItem {
            text: "4:3"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.Aspect_4_3
            onTriggered: root.videoSurface.aspectMode = VideoSurface.Aspect_4_3
        }
        MenuItem {
            text: "2.35:1 (Cinema)"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.Aspect_235_1
            onTriggered: root.videoSurface.aspectMode = VideoSurface.Aspect_235_1
        }
        MenuItem {
            text: "Stretch"
            checkable: true
            checked: root.videoSurface && root.videoSurface.aspectMode === VideoSurface.AspectStretch
            onTriggered: root.videoSurface.aspectMode = VideoSurface.AspectStretch
        }
    }

    // --- Zoom submenu ---
    Menu {
        title: "Zoom"
        MenuItem {
            text: "100%"
            onTriggered: { root.videoSurface.zoomFactor = 1.0; root.videoSurface.panX = 0; root.videoSurface.panY = 0 }
        }
        MenuItem {
            text: "150%"
            onTriggered: root.videoSurface.zoomFactor = 1.5
        }
        MenuItem {
            text: "200%"
            onTriggered: root.videoSurface.zoomFactor = 2.0
        }
        MenuItem {
            text: "400%"
            onTriggered: root.videoSurface.zoomFactor = 4.0
        }
        MenuSeparator {}
        MenuItem {
            text: "Zoom In"
            onTriggered: root.videoSurface.zoomFactor = Math.min(8.0, root.videoSurface.zoomFactor + 0.25)
        }
        MenuItem {
            text: "Zoom Out"
            onTriggered: root.videoSurface.zoomFactor = Math.max(1.0, root.videoSurface.zoomFactor - 0.25)
        }
    }

    MenuSeparator {}

    // --- Screenshot / clipboard (real, via ScreenCaptureController) ---
    MenuItem {
        text: "📸  Save Screenshot"
        enabled: playback.hasMedia
        onTriggered: {
            capture.captureToFile(playback, playback.mediaTitle)
            // showSaveToast below is optional; the controller exposes
            // lastSavePath / lastError for the UI to render.
        }
    }
    MenuItem {
        text: "📋  Copy Frame to Clipboard"
        enabled: playback.hasMedia
        onTriggered: capture.captureToClipboard(playback)
    }

    MenuSeparator {}

    // --- A/B compare (real, via CompareController) ---
    MenuItem {
        text: compare.active ? "⇄  Disable A/B Compare" : "⇄  Enable A/B Compare"
        enabled: playback.hasMedia
        onTriggered: compare.active = !compare.active
    }
    MenuItem {
        text: "Swap A/B Sides"
        enabled: compare.active
        onTriggered: compare.swapSides()
    }

    MenuSeparator {}

    // --- Playlist transport + file ops ---
    MenuItem {
        text: "⏮  Previous item (or restart)"
        enabled: playback.hasMedia
        onTriggered: playback.previous()
    }
    MenuItem {
        text: "⏭  Next item"
        enabled: playback.hasNext
        onTriggered: playback.next()
    }
    MenuItem {
        text: "☷  Show Playlist"
        onTriggered: if (root.playlistPopup) root.playlistPopup.open()
    }
    MenuItem {
        text: "📁  Open Videos…"
        onTriggered: openDialog.open()
    }
    MenuItem {
        text: "↻  Reload"
        enabled: playback.hasMedia
        onTriggered: {
            const path = playback.mediaInfo.path || ""
            if (path.length > 0)
                playback.openUrl("file://" + path)
        }
    }

    MenuSeparator {}

    MenuItem {
        text: window.visibility === Window.FullScreen ? "⛶  Exit Fullscreen" : "⛶  Fullscreen"
        onTriggered: window.visibility === Window.FullScreen
                     ? window.showNormal() : window.showFullScreen()
    }
    MenuItem {
        text: "⚙  Settings…"
        onTriggered: settingsDialog.show()
    }
}
