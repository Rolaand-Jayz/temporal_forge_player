// LabeledSlider.qml — labeled slider with live value readout.
//
// Drops into a ColumnLayout / SettingsSection. Renders the label on the left,
// the value on the right, and the slider below.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    Layout.fillWidth: true
    property string label: ""
    property string suffix: ""
    property real value: 0
    property real from: 0
    property real to: 1
    property real stepSize: 0.1
    property int decimals: 2
    property bool enabled: true
    signal moved(real newValue)

    // Keep the label row and the interactive track in separate vertical
    // bands. The old 44 px height placed both at the same y-position and
    // made adjacent settings unreadable and difficult to click.
    implicitHeight: 68
    height: 68

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            spacing: 8

            Text {
                text: root.label
                color: root.enabled ? "#b0b0bc" : "#555560"
                font.pixelSize: 12
                Layout.fillWidth: true
            }
            Text {
                text: root.value.toFixed(root.decimals) + root.suffix
                color: root.enabled ? "#e44d3a" : "#555560"
                font.pixelSize: 11
                font.family: "monospace"
                horizontalAlignment: Qt.AlignRight
            }
        }

        Slider {
            id: control
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            from: root.from
            to: root.to
            value: root.value
            stepSize: root.stepSize
            enabled: root.enabled
            onMoved: root.moved(value)

            background: Rectangle {
                x: control.leftPadding
                y: control.topPadding + control.availableHeight / 2 - height / 2
                implicitWidth: 200
                implicitHeight: 4
                width: control.availableWidth
                height: implicitHeight
                radius: 2
                color: "#2a2a32"

                Rectangle {
                    width: control.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: root.enabled ? "#e44d3a" : "#555560"
                }
            }
            handle: Rectangle {
                x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
                y: control.topPadding + control.availableHeight / 2 - height / 2
                implicitWidth: 14
                implicitHeight: 14
                radius: 7
                color: control.pressed ? "#ffffff" : "#e8e8ee"
                border.color: "#e44d3a"
                border.width: 2
            }
        }
    }
}
