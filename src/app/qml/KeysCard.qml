import QtQuick
import QtQuick.Controls

// The 3D view's keys for the current controls preset, grouped, from ViewportItem.keyHelp() (view_keys.h).
Column {
    id: list
    required property var rows
    width: parent ? parent.width : 300
    spacing: 2
    bottomPadding: 4

    Repeater {
        model: list.rows
        Column {
            required property var modelData
            required property int index
            width: list.width
            Label {
                visible: index === 0 || list.rows[index - 1].group !== modelData.group
                topPadding: index ? 8 : 0
                bottomPadding: 2
                text: modelData.group.toUpperCase()
                color: Theme.muted
                font.pixelSize: 8
                font.letterSpacing: 1.2
            }
            Row {
                spacing: 8
                Label {
                    width: 130
                    text: modelData.keys
                    color: Theme.accent
                    font.family: Theme.mono
                    font.pixelSize: Theme.small + 1
                    wrapMode: Text.Wrap
                }
                Label {
                    width: list.width - 138
                    text: modelData.label
                    color: Theme.fg
                    font.pixelSize: Theme.small + 1
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
