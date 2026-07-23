// SettingsSection.qml — grouped control container with title + description.
import QtQuick
import QtQuick.Layouts

Item {
    id: root
    Layout.fillWidth: true
    property string title: ""
    property string description: ""
    default property alias content: contentCol.children

    // Auto-size to the inner column.
    implicitHeight: outerCol.implicitHeight
    height: implicitHeight

    ColumnLayout {
        id: outerCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 6

        // Title bar with accent rule.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 3
                Layout.preferredHeight: 14
                color: "#e44d3a"
                radius: 1
            }
            Text {
                text: root.title
                color: "#e8e8ee"
                font.pixelSize: 13
                font.bold: true
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.description
            visible: root.description.length > 0
            color: "#6a6a75"
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        ColumnLayout {
            id: contentCol
            Layout.fillWidth: true
            spacing: 8
        }
    }
}
