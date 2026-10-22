import QtQuick
import QtQuick.Controls

ToolButton {
    id: b
    property bool active: false
    width: 28
    height: 28
    icon.width: 18
    icon.height: 18
    icon.color: active ? Theme.accentOn : Theme.muted
    display: AbstractButton.IconOnly
    ToolTip.visible: hovered
    ToolTip.text: text
    ToolTip.delay: 500
    background: Rectangle {
        radius: 6
        color: b.active ? Theme.accent : b.hovered ? Theme.line : "transparent"
    }
}
