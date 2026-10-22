import QtQuick
import QtQuick.Controls

// CheckBox with an indicator that reads on dark panels.
CheckBox {
    id: c
    font.pixelSize: Theme.small + 1
    indicator: Rectangle {
        x: c.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 14
        implicitHeight: 14
        radius: 3
        color: c.checked ? Theme.accent : Theme.field
        border.color: c.checked || c.visualFocus ? Theme.accent : Theme.muted
        Label {
            anchors.centerIn: parent
            visible: c.checked
            text: "✓"
            color: Theme.accentOn
            font.pixelSize: 11
        }
    }
}
