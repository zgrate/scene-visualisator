import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One inspector row: label plus one or more numeric/text inputs. Each input commits on Enter or focus loss
// through committed(index, text); the caller turns that into a Session edit.
RowLayout {
    id: row
    property string label
    property var values: []   // display strings, one per input
    property bool numeric: true
    property bool warn: false
    signal committed(int index, string text)

    width: parent ? parent.width : 0
    spacing: 4
    Label {
        text: row.label
        color: Theme.muted
        font.pixelSize: Theme.small + 1
        Layout.preferredWidth: 62
    }
    Repeater {
        model: row.values.length
        TextField {
            required property int index
            text: row.values[index] ?? ""
            Layout.fillWidth: true
            Layout.preferredWidth: 10
            implicitHeight: 22
            padding: 3
            font.family: row.numeric ? Theme.mono : Qt.application.font.family
            font.pixelSize: Theme.small + 1
            color: row.warn ? Theme.warn : Theme.fg
            horizontalAlignment: row.numeric ? TextInput.AlignRight : TextInput.AlignLeft
            selectByMouse: true
            background: Rectangle {
                color: Theme.field
                radius: 3
                border.color: parent.activeFocus ? Theme.accent : (row.warn ? Theme.warn : Theme.line)
            }
            onEditingFinished: if (text !== row.values[index]) row.committed(index, text)
            Keys.onEscapePressed: { text = row.values[index]; focus = false }
        }
    }
}
