// LiveHud.qml — compact enhancement status indicator (bottom-right).
//
// Single-line pill showing backend + resolution chain + status dot.
// Toggle with 'H'. Defaults visible.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: hud
    visible: playback.hasMedia && hudVisible
    property bool hudVisible: true

    anchors.bottom: parent.bottom
    anchors.bottomMargin: 104
    anchors.right: parent.right
    anchors.rightMargin: 12
    width: hudText.implicitWidth + 24
    height: 22
    color: "#000000"
    opacity: 0.72
    radius: 11
    z: 50

    Row {
        id: hudRow
        anchors.centerIn: parent
        spacing: 5

        // Status dot.
        Rectangle {
            width: 7; height: 7; radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: {
                if (!playback.hasMedia) return "#555560"
                if (playback.fsr4Active && playback.fsr4ProofPassed) return "#39d353"
                if (playback.fsr4Active) return "#d29922"
                return "#6a6a75"
            }
        }

        Text {
            id: hudText
            text: {
                if (!playback.hasMedia) return ""
                var res = fsr.sourceWidth + "x" + fsr.sourceHeight
                if (playback.fsr4Active && playback.fsr4OutputWidth > 0)
                    res += " → " + playback.fsr4OutputWidth + "x" + playback.fsr4OutputHeight
                if (playback.lastFsr4DispatchMs > 0)
                    res += "  " + playback.lastFsr4DispatchMs.toFixed(1) + "ms"
                return res
            }
            color: "#c8c8d0"
            font.pixelSize: 10
            font.family: "monospace"
        }
    }
}
