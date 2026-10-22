import QtQuick
import QtQuick.Controls

// A compact text field in the Studio tokens; stock Fusion fields vanish against the dark panels.
TextField {
    implicitHeight: 24
    padding: 4
    font.pixelSize: Theme.small + 1
    color: Theme.fg
    selectByMouse: true
    background: Rectangle {
        color: Theme.field
        radius: 3
        border.color: parent.activeFocus ? Theme.accent : Theme.line
    }
}
