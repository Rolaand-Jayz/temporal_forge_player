// LabeledToggle.qml — labeled checkbox-style toggle for the Settings dialog.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    Layout.fillWidth: true
    property string text: ""
    property bool checked: false
    property string description: ""
    signal toggled(bool newChecked)

    implicitHeight: 42
    height: 42

    RowLayout {
        anchors.fill: parent
        spacing: 10

        Switch {
            id: control
            checked: root.checked
            onToggled: root.toggled(checked)
            Layout.preferredWidth: 50
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                text: root.text
                color: "#e8e8ee"
                font.pixelSize: 12
            }
            Text {
                text: root.description
                visible: root.description.length > 0
                color: "#6a6a75"
                font.pixelSize: 9
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
    }
}
