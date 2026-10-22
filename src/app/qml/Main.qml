import QtQuick
import QtQuick.Controls
import SceneVisualiser

ApplicationWindow {
    width: 1440
    height: 900
    visible: true
    title: qsTr("Scene Visualiser")
    color: "#121418" // shortcut: literal until the M5 Theme singleton exists

    ViewportItem {
        anchors.fill: parent
    }
}
