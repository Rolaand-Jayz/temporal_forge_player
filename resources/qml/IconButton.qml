// IconButton.qml — reusable icon/text button for the control overlay.
//
// Designed to drop into a RowLayout. Highlights on hover, shows a tooltip on
// hover, supports a "primary" state (the play/pause button is bigger and
// brighter) and a "checked" state (compare, fullscreen). Used by Main.qml's
// control bar so every transport button looks and behaves the same way.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // The text shown inside the button. Can be a glyph or (with showLabel)
    // a short string like "1.00x".
    property string glyph: ""
    property string tooltip: ""
    property bool primary: false
    property bool checked: false
    property bool showLabel: false
    signal clicked()

    implicitWidth: showLabel ? label.implicitWidth + 16 : 36
    implicitHeight: 36
    Layout.preferredHeight: 36

    // Hover detection drives the highlight + tooltip + auto-hide hold-off.
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        enabled: root.enabled
        onClicked: root.clicked()
    }

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 6
        color: {
            if (!root.enabled) return "transparent"
            if (ma.pressed) return "#2a2a32"
            if (root.checked) return "#3a1f1c"
            if (ma.containsMouse) return "#1e1e24"
            return "transparent"
        }
        border.color: root.checked ? "#e44d3a" : "transparent"
        border.width: root.checked ? 1 : 0
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.glyph
        color: {
            if (!root.enabled) return "#555560"
            if (root.checked) return "#ffffff"
            if (root.primary) return "#ffffff"
            if (ma.containsMouse) return "#ffffff"
            return "#d8d8de"
        }
        font.pixelSize: root.primary ? 22 : (root.showLabel ? 12 : 17)
        font.bold: root.primary || root.showLabel
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
    }

    // Tooltip on hover — purely declarative, no native Tooltip dependency.
    ToolTip.visible: ma.containsMouse && root.tooltip.length > 0
    ToolTip.delay: 400
    ToolTip.text: root.tooltip
}
