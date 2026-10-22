import QtQuick
import QtQuick.Controls

Button {
    id: b
    property bool active: false
    implicitHeight: 22
    leftPadding: 8
    rightPadding: 8
    font.pixelSize: Theme.small + 1
    contentItem: Label {
        text: b.text
        font: b.font
        color: b.active ? Theme.accentOn : Theme.fg
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 4
        color: b.active ? Theme.accent : b.hovered ? Theme.line : Theme.panel
        border.color: Theme.line
    }
}
