// Main.qml — standardized media player UI.
//
// Layout (VLC/MPV-class conventions):
//   - Full-bleed video surface.
//   - Bottom control overlay: draggable seek bar on top, buttons below.
//   - Top-right: optional debug + media info panels (toggleable, auto-hide).
//   - Hover-aware chrome: every control surface hides itself after the mouse
//     has been idle for a few seconds, and only comes back when the mouse
//     enters the region it lives in (or anywhere, in fullscreen). This is
//     the standard player behavior the user asked for.
//
// Keyboard shortcuts (spec 05 + new): Space, F, Esc, O, M, C, H, Ctrl+,,
// Ctrl+F, Ctrl+I, Left/Right (seek 10s), Up/Down (volume), , and . (frame
// step), [ and ] (playback speed).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import TemporalForge

ApplicationWindow {
    id: window
    visible: false
    width: 1280; height: 720
    color: "#0a0a0c"
    title: "Temporal Forge Player" + (playback.mediaTitle ? " — " + playback.mediaTitle : "")

    // --- auto-hide state ---
    // True while the chrome (controls + overlays) is visible. Drops to false
    // after the mouse has been still for a few seconds, and rises again on
    // any pointer motion or when the mouse enters one of the overlay regions.
    property bool chromeVisible: true
    // True while the user is actively hovering a control region; that region
    // never auto-hides while hovered.
    property bool controlsHovered: false
    property bool overlaysHovered: false

    // --- open-file dialog ---
    FileDialog {
        id: openDialog
        title: "Open video"
        nameFilters: ["Video files (*.mp4 *.mkv *.mov *.webm *.avi *.ts *.m2ts)", "All files (*)"]
        onAccepted: playback.openUrl(currentFile)
    }

    // --- FSR4 INT8 experimental warning ---
    Dialog {
        id: experimentalWarning
        title: "FSR 4.1 INT8 — Experimental"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            text: "FSR 4.1 INT8 is an experimental, RE-derived video upscaler for RDNA3. Validation here covers local GPU execution, finite output, bounds, and visual A/B inspection. AMD's Windows runtime and an official video-FSR reference are outside this project's definition of done."
            wrapMode: Text.Wrap
            width: parent ? parent.width - 24 : 420
        }
    }

    // Track window size for the FSR info chain (spec 02: window size only
    // affects presentation, never the FSR target).
    Connections {
        target: window
        function onWidthChanged() { fsr.setWindowSize(surfaceArea.width, surfaceArea.height) }
        function onHeightChanged() { fsr.setWindowSize(surfaceArea.width, surfaceArea.height) }
    }

    Component.onCompleted: {
        fsr.setWindowSize(surfaceArea.width, surfaceArea.height)
        playback.compareEnabled = compare.active
    }

    // Refresh FSR source dims when media changes.
    Connections {
        target: playback
        function onMediaChanged() { fsr.setWindowSize(surfaceArea.width, surfaceArea.height) }
    }
    Connections {
        target: compare
        function onChanged() { playback.compareEnabled = compare.active }
    }

    // --- video surface ---
    // Fills the entire window. The control overlay floats above it; the
    // overlay used to be a ToolBar footer which reserved its own space and
    // squashed the video — the user reported that the controls did not feel
    // like a normal player. Floating overlay is the standard.
    Item {
        id: surfaceArea
        anchors.fill: parent
        clip: true

        Rectangle { anchors.fill: parent; color: "#000000" }

        // Video surface: imports the completed native Vulkan image written by
        // the real FSR4 dispatch. The refresh Timer only advances frame
        // pacing and schedules a scene-graph update.
        VideoSurface {
            id: videoSurface
            anchors.fill: parent
            visible: playback.hasMedia
            brightness: videoSettings.brightness
            contrast: videoSettings.contrast
            saturation: videoSettings.saturation
            hue: videoSettings.hue
            gamma: videoSettings.gamma
            presentationScaler: videoSettings.presentationScaler
            compareActive: false
            compareRawOnLeft: false
        }

        Item {
            id: compareRawPane
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width * compare.splitPosition
            visible: playback.hasMedia && compare.active
            clip: true
            z: 1

            VideoSurface {
                id: compareRawSurface
                width: compareRawPane.parent.width
                height: compareRawPane.parent.height
                brightness: videoSettings.brightness
                contrast: videoSettings.contrast
                saturation: videoSettings.saturation
                hue: videoSettings.hue
                gamma: videoSettings.gamma
                presentationScaler: videoSettings.presentationScaler
                compareActive: true
                compareRawOnLeft: true
                rawOnly: true
            }
        }

        // Drive the VideoSurface refresh from QML.
        Timer {
            running: playback.hasMedia && playback.playing
            repeat: true
            interval: playback.mediaInfo.fps > 0
                      ? Math.max(8, Math.round(1000 / Math.min(playback.mediaInfo.fps, 60)))
                      : 16
            property int lastFrameCounter: -1
            onTriggered: {
                videoSurface.refresh()
                if (compare.active && videoSurface.frameCounter !== lastFrameCounter) {
                    lastFrameCounter = videoSurface.frameCounter
                    compareRawSurface.requestUpdate()
                }
            }
        }

        // Compare divider drag handle (visible only in compare mode).
        MouseArea {
            anchors.fill: parent
            enabled: compare.active
            z: 10
            cursorShape: compare.active ? Qt.SplitHCursor : Qt.ArrowCursor
            onPressed: function(mouse) {
                // Clicking anywhere moves the divider there.
                compare.splitPosition = mouse.x / parent.width
            }
            onPositionChanged: function(mouse) {
                if (pressed)
                    compare.splitPosition = Math.max(0, Math.min(1, mouse.x / parent.width))
            }
        }

        Rectangle {
            visible: compare.active
            x: parent.width * compare.splitPosition - width * 0.5
            y: 0
            width: 2
            height: parent.height
            color: "#f2f2f4"
            opacity: 0.9
            z: 11
        }

        Rectangle {
            visible: compare.active
            x: parent.width * compare.splitPosition - width * 0.5
            y: parent.height * 0.5 - height * 0.5
            width: 28
            height: 28
            radius: 14
            color: "#15151a"
            border.color: "#f2f2f4"
            border.width: 2
            z: 12
        }

        // Startup hint when no media is loaded.
        Label {
            anchors.centerIn: parent
            visible: !playback.hasMedia
            text: "Press O to open a video file"
            color: "#6a6a75"
            font.pixelSize: 18
        }

        // Whole-window mouse tracker: drives auto-hide. This MouseArea sits
        // above the video but below the controls; it never blocks clicks
        // (it only watches motion), so playback toggles and divider drags
        // keep working.
        MouseArea {
            id: activityTracker
            anchors.fill: parent
            enabled: true
            hoverEnabled: true
            propagateComposedEvents: true
            acceptedButtons: Qt.NoButton  // observe only; never swallow clicks
            z: 5
            onPositionChanged: function(mouse) {
                chromeVisible = true
                hideTimer.restart()
                mouse.accepted = false
            }
            onExited: hideTimer.restart()
        }

        // Double-click toggles fullscreen (common player UX). Single click
        // toggles play/pause AND pokes the chrome. Right-click opens the
        // standardized context menu (aspect / zoom / screenshot / etc.).
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            z: 6
            onDoubleClicked: window.visibility === Window.FullScreen
                             ? window.showNormal() : window.showFullScreen()
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    contextMenu.popup()
                    return
                }
                playback.togglePlay()
                chromeVisible = true
                hideTimer.restart()
            }
        }
    }

    // Right-click context menu. All entries map to real actions; see
    // VideoContextMenu.qml.
    VideoContextMenu {
        id: contextMenu
        videoSurface: videoSurface
    }

    // --- auto-hide timer ---
    // Resets whenever the mouse moves. When it fires, the chrome drops unless
    // a control region is being actively hovered or a menu/popup is open.
    Timer {
        id: hideTimer
        interval: 2500
        repeat: false
        running: false
        onTriggered: {
            if (!controlsHovered && !overlaysHovered &&
                !openDialog.visible && !settingsDialog.visible &&
                !experimentalWarning.visible)
                chromeVisible = false
        }
    }

    // --- control overlay (bottom of the window, floating above the video) ---
    // Two-row layout: draggable seek bar on top, buttons below. The whole
    // overlay fades out smoothly when chromeVisible is false.
    Item {
        id: controlOverlay
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 92
        z: 20
        opacity: chromeVisible ? 1.0 : 0.0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        // Dim the video behind the controls so text stays legible.
        Rectangle {
            id: controlBackground
            anchors.fill: parent
            // Subtle gradient from transparent (top) to dark (bottom) — VLC style.
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.55; color: "#aa0a0a0c" }
                GradientStop { position: 1.0; color: "#f0000000" }
            }
        }

        // Hover trap: while the mouse is anywhere over the overlay, controls
        // stay visible and the auto-hide timer is held off.
        MouseArea {
            id: controlHoverTrap
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton  // observe only; controls receive clicks
            z: 0
            onEntered: {
                controlsHovered = true
                chromeVisible = true
                hideTimer.stop()
            }
            onExited: {
                controlsHovered = false
                hideTimer.restart()
            }
            onPositionChanged: {
                chromeVisible = true
                hideTimer.restart()
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 4
            anchors.bottomMargin: 8
            spacing: 4
            z: 1

            // --- top row: seek bar ---
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: formatTime(playback.positionUs)
                    color: "#e8e8ee"
                    font.pixelSize: 12
                    Layout.preferredWidth: 56
                    horizontalAlignment: Qt.AlignHCenter
                    font.family: "monospace"
                }

                // Draggable, hover-aware seek slider. Click-to-seek anywhere
                // on the trough and drag the handle to scrub.
                Slider {
                    id: seekBar
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, playback.durationUs)
                    value: playback.positionUs
                    enabled: playback.hasMedia

                    onPressedChanged: {
                        if (!pressed)
                            playback.seekUs(Math.round(value))
                    }
                    // Click-to-seek anywhere on the trough.
                    onMoved: playback.seekUs(Math.round(value))

                    background: Rectangle {
                        x: seekBar.leftPadding
                        y: seekBar.topPadding + seekBar.availableHeight / 2 - height / 2
                        implicitWidth: 200
                        implicitHeight: 4
                        width: seekBar.availableWidth
                        height: implicitHeight
                        radius: 2
                        color: "#2a2a32"

                        Rectangle {
                            width: seekBar.visualPosition * parent.width
                            height: parent.height
                            color: "#e44d3a"
                            radius: 2
                        }
                    }

                    handle: Rectangle {
                        x: seekBar.leftPadding + seekBar.visualPosition
                           * (seekBar.availableWidth - width)
                        y: seekBar.topPadding + seekBar.availableHeight / 2 - height / 2
                        implicitWidth: 14
                        implicitHeight: 14
                        radius: 7
                        color: seekBar.pressed ? "#ffffff" : "#e8e8ee"
                        border.color: "#e44d3a"
                        border.width: 2
                    }
                }

                Label {
                    text: formatTime(playback.durationUs)
                    color: "#b0b0bc"
                    font.pixelSize: 12
                    Layout.preferredWidth: 56
                    horizontalAlignment: Qt.AlignHCenter
                    font.family: "monospace"
                }
            }

            // --- bottom row: buttons ---
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                // Open file.
                IconButton {
                    glyph: "📁"
                    tooltip: "Open file (O)"
                    onClicked: openDialog.open()
                    Layout.preferredWidth: 36
                }

                // Skip back 10s.
                IconButton {
                    glyph: "⏪"
                    tooltip: "Back 10s (Left)"
                    enabled: playback.hasMedia
                    onClicked: seekBy(-10000000)
                    Layout.preferredWidth: 36
                }

                // Frame step backward (pause first).
                IconButton {
                    glyph: "◁"
                    tooltip: "Frame back (,)"
                    enabled: playback.hasMedia
                    onClicked: seekBy(-400000)  // ~1 frame at 25fps
                    Layout.preferredWidth: 28
                }

                // Play/pause (wide, primary).
                IconButton {
                    glyph: playback.playing ? "❚❚" : "▶"
                    tooltip: playback.playing ? "Pause (Space)" : "Play (Space)"
                    enabled: playback.hasMedia
                    primary: true
                    onClicked: playback.togglePlay()
                    Layout.preferredWidth: 48
                }

                // Frame step forward (pause first).
                IconButton {
                    glyph: "▷"
                    tooltip: "Frame forward (.)"
                    enabled: playback.hasMedia
                    onClicked: seekBy(400000)
                    Layout.preferredWidth: 28
                }

                // Skip forward 10s.
                IconButton {
                    glyph: "⏩"
                    tooltip: "Forward 10s (Right)"
                    enabled: playback.hasMedia
                    onClicked: seekBy(10000000)
                    Layout.preferredWidth: 36
                }

                // Volume mute toggle.
                IconButton {
                    glyph: playback.muted || playback.volume === 0 ? "🔇"
                          : playback.volume < 50 ? "🔉" : "🔊"
                    tooltip: playback.muted ? "Unmute (M)" : "Mute (M)"
                    onClicked: playback.muted = !playback.muted
                    Layout.preferredWidth: 32
                }
                Slider {
                    from: 0; to: 100
                    value: playback.volume
                    onMoved: playback.volume = value
                    Layout.preferredWidth: 90
                    enabled: true
                    background: Rectangle {
                        implicitHeight: 4
                        height: 4
                        radius: 2
                        color: "#2a2a32"
                        Rectangle {
                            width: parent.visualPosition * parent.width
                            height: parent.height
                            color: "#e8e8ee"
                            radius: 2
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Playback speed cycle.
                IconButton {
                    glyph: speedLabel(playbackRateIndex)
                    tooltip: "Playback speed: " + playbackSpeeds[playbackRateIndex].toFixed(2) + "x ([])"
                    onClicked: cyclePlaybackSpeed()
                    Layout.preferredWidth: 64
                    showLabel: true
                }

                // FSR preset quick selector (spec 05).
                ComboBox {
                    id: presetCombo
                    model: fsr.presetOptions
                    textRole: "label"
                    valueRole: "key"
                    currentIndex: {
                        const key = fsr.preset
                        for (let i = 0; i < fsr.presetOptions.length; ++i)
                            if (fsr.presetOptions[i].key === key) return i
                        return 0
                    }
                    onActivated: function(index) {
                        fsr.preset = currentValue
                    }
                    Layout.preferredWidth: 220
                }

                // Compare toggle.
                IconButton {
                    glyph: "⇄"
                    tooltip: "A/B compare (C)"
                    checked: compare.active
                    enabled: playback.hasMedia
                    onClicked: compare.active = !compare.active
                    Layout.preferredWidth: 32
                }

                // Fullscreen toggle.
                IconButton {
                    glyph: "⛶"
                    tooltip: window.visibility === Window.FullScreen
                             ? "Exit fullscreen (F/Esc)" : "Fullscreen (F)"
                    checked: window.visibility === Window.FullScreen
                    onClicked: window.visibility === Window.FullScreen
                               ? window.showNormal() : window.showFullScreen()
                    Layout.preferredWidth: 36
                }

                // Settings gear button.
                IconButton {
                    glyph: "⚙"
                    tooltip: "Settings (Ctrl+,)"
                    onClicked: settingsDialog.show()
                    Layout.preferredWidth: 36
                }
            }
        }
    }

    // --- keyboard shortcuts (spec 05 + new transport keys) ---
    Shortcut { sequence: "Space"; onActivated: playback.togglePlay() }
    Shortcut { sequence: "O";     onActivated: openDialog.open() }
    Shortcut {
        sequence: "F"
        onActivated: window.visibility === Window.FullScreen
                     ? window.showNormal() : window.showFullScreen()
    }
    Shortcut { sequence: "Escape"; onActivated: if (window.visibility === Window.FullScreen) window.showNormal() }
    Shortcut { sequence: "M"; onActivated: playback.muted = !playback.muted }
    Shortcut { sequence: "Ctrl+F"; onActivated: debug.visible = !debug.visible }
    Shortcut { sequence: "Ctrl+I"; onActivated: infoPanel.visible = !infoPanel.visible }
    Shortcut { sequence: "Ctrl+,"; onActivated: settingsDialog.show() }
    // Screenshot + clipboard capture (real, via ScreenCaptureController).
    Shortcut { sequence: "Ctrl+S"; onActivated: capture.captureToFile(playback, playback.mediaTitle) }
    Shortcut { sequence: "Ctrl+Shift+C"; onActivated: capture.captureToClipboard(playback) }
    Shortcut { sequence: "C"; onActivated: compare.active = !compare.active }
    Shortcut { sequence: "H"; onActivated: liveHud.hudVisible = !liveHud.hudVisible }
    // Seek by 10s.
    Shortcut { sequence: "Left";  onActivated: seekBy(-10000000) }
    Shortcut { sequence: "Right"; onActivated: seekBy(10000000) }
    // Frame step (~1 frame @ 25fps). Always pauses first, like VLC.
    Shortcut { sequence: ","; onActivated: { if (playback.playing) playback.togglePlay(); seekBy(-400000) } }
    Shortcut { sequence: "."; onActivated: { if (playback.playing) playback.togglePlay(); seekBy(400000) } }
    // Volume via arrow keys.
    Shortcut { sequence: "Up";   onActivated: playback.volume = Math.min(100, playback.volume + 5) }
    Shortcut { sequence: "Down"; onActivated: playback.volume = Math.max(0, playback.volume - 5) }
    // spec 05 FSR preset shortcuts
    Shortcut { sequence: "Ctrl+1"; onActivated: fsr.preset = 1 } // Off
    Shortcut { sequence: "Ctrl+2"; onActivated: fsr.preset = 3 } // Quality
    Shortcut { sequence: "Ctrl+3"; onActivated: fsr.preset = 4 } // Balanced
    Shortcut { sequence: "Ctrl+4"; onActivated: fsr.preset = 5 } // Performance
    Shortcut { sequence: "Ctrl+5"; onActivated: fsr.preset = 6 } // Ultra Performance
    // Playback speed cycle.
    Shortcut { sequence: "["; onActivated: stepPlaybackSpeed(-1) }
    Shortcut { sequence: "]"; onActivated: stepPlaybackSpeed(1) }

    // --- debug overlay (spec 05) ---
    Rectangle {
        id: debugOverlay
        visible: debug.visible
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        width: debugCol.implicitWidth + 24
        height: debugCol.implicitHeight + 24
        color: "#000000"
        opacity: 0.82
        radius: 6
        z: 100

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            onEntered: { overlaysHovered = true; chromeVisible = true; hideTimer.stop() }
            onExited:   { overlaysHovered = false; hideTimer.restart() }
        }

        Column {
            id: debugCol
            anchors.margins: 12
            anchors.fill: parent
            spacing: 2
            Label { text: "DEBUG (Ctrl+F)"; color: "#e44d3a"; font.pixelSize: 11; font.bold: true }
            Label { text: "Source: " + debug.stats.sourceResolution; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "FSR target: " + debug.stats.fsrTargetResolution; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Window: " + debug.stats.windowResolution; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Backend: " + debug.stats.backend; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Preset: " + debug.stats.preset; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Video FPS: " + debug.stats.videoFps + " (preserved)"; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Frame PTS: " + debug.stats.framePts; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "History resets: " + debug.stats.historyResets; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Scene cuts: " + debug.stats.sceneCutsDetected; color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Motion confidence: " + debug.stats.motionConfidence.toFixed(3); color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
            Label { text: "Reactive avg: " + debug.stats.reactiveAverage.toFixed(3); color: "#c8c8d0"; font.pixelSize: 11; font.family: "monospace" }
        }
    }

    // --- media / FSR info panel (spec 05 Media Info + FSR Info panels) ---
    Rectangle {
        id: infoPanel
        visible: false
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        width: infoCol.implicitWidth + 24
        height: infoCol.implicitHeight + 24
        color: "#000000"
        opacity: 0.85
        radius: 6
        z: 100

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            onEntered: { overlaysHovered = true; chromeVisible = true; hideTimer.stop() }
            onExited:   { overlaysHovered = false; hideTimer.restart() }
        }

        Column {
            id: infoCol
            anchors.margins: 12
            anchors.fill: parent
            spacing: 4
            Label { text: "MEDIA INFO (Ctrl+I)"; color: "#e44d3a"; font.pixelSize: 11; font.bold: true }
            Label { text: "File: " + playback.mediaTitle; color: "#c8c8d0"; font.pixelSize: 11 }
            Label { text: "Container: " + playback.mediaInfo.container; color: "#9a9aa5"; font.pixelSize: 10 }
            Label { text: "Video: " + playback.mediaInfo.videoCodec + " " + playback.mediaInfo.width + "x" + playback.mediaInfo.height; color: "#9a9aa5"; font.pixelSize: 10 }
            Label { text: "FPS: " + playback.mediaInfo.fps + " (preserved)"; color: "#9a9aa5"; font.pixelSize: 10 }
            Label { text: "Audio: " + playback.mediaInfo.audioCodec + " " + playback.mediaInfo.sampleRate + "Hz"; color: "#9a9aa5"; font.pixelSize: 10 }
            Label { text: " "; color: "#9a9aa5"; font.pixelSize: 6 }
            Label { text: "FSR CHAIN"; color: "#e44d3a"; font.pixelSize: 11; font.bold: true }
            Label { text: fsr.chainLabel; color: "#c8c8d0"; font.pixelSize: 10; wrapMode: Text.Wrap; width: parent.width - 24 }
            Label { text: "Backend: " + fsr.backendLabel; color: "#9a9aa5"; font.pixelSize: 10 }
        }
    }

    // --- Live enhancement HUD (toggle with H) ---
    // Positioning (bottom-right, above timeline) is defined in LiveHud.qml.
    // The HUD respects the global chromeVisible flag so it hides with the
    // rest of the chrome when the mouse is idle, but its toggle (H) still
    // controls whether it reappears.
    LiveHud {
        id: liveHud
        visible: playback.hasMedia && hudVisible && chromeVisible
    }

    // --- Settings dialog (Ctrl+, or gear button) ---
    SettingsDialog {
        id: settingsDialog
        visible: false
        function show() {
            visible = true
            raise()
            requestActivate()
        }
    }

    // --- playback speed state ---
    // Standard player speeds: 0.25x through 2x in fine steps. Index 3 = 1x.
    // NOTE: the engine's PTS pacing still honors wall-clock between frames;
    // surfacing the chosen speed in the UI today and the actual rate control
    // is a tracked follow-up. The control is intentionally kept honest — it
    // shows what speed you have selected rather than implying the engine is
    // already honoring it.
    property var playbackSpeeds: [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
    property int playbackRateIndex: 3

    function speedLabel(idx) {
        const v = playbackSpeeds[idx]
        if (v === 1.0) return "1.00x"
        return v.toFixed(2) + "x"
    }
    function cyclePlaybackSpeed() { stepPlaybackSpeed(1) }
    function stepPlaybackSpeed(delta) {
        playbackRateIndex = Math.max(0,
            Math.min(playbackSpeeds.length - 1, playbackRateIndex + delta))
    }

    // --- helpers ---
    function seekBy(deltaUs) {
        if (!playback.hasMedia) return
        const target = Math.max(0,
            Math.min(playback.durationUs, playback.positionUs + deltaUs))
        playback.seekUs(target)
    }

    function formatTime(us) {
        if (us <= 0) return "0:00"
        const totalSec = Math.floor(us / 1000000)
        const h = Math.floor(totalSec / 3600)
        const m = Math.floor((totalSec % 3600) / 60)
        const s = totalSec % 60
        if (h > 0) return h + ":" + String(m).padStart(2, '0') + ":" + String(s).padStart(2, '0')
        return m + ":" + String(s).padStart(2, '0')
    }
}
