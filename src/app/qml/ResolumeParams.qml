import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every Arena param of an object, output or the composition ("composition"), as Arena's text (B3). Collapsed by
// default; each change is one undoable Session.setResolumeParam.
Column {
    id: rp
    property string uuid
    property bool open: false
    property string error
    // Reading Session.objects re-evaluates the list after every document change.
    readonly property var params: open && uuid ? (Session.objects, Session.resolumeParams(uuid)) : []
    width: parent ? parent.width : 0
    spacing: 3

    function commit(key, value) {
        error = Session.setResolumeParam(uuid, key, value)
        if (error) Session.changed() // re-read the stored value
    }

    Label {
        text: (rp.open ? "▾ " : "▸ ") + qsTr("RESOLUME")
        color: Theme.muted
        font.pixelSize: Theme.small
        font.letterSpacing: 1
        topPadding: 6
        TapHandler { onTapped: rp.open = !rp.open }
    }
    Label {
        visible: rp.error !== ""
        text: rp.error
        color: Theme.err
        font.pixelSize: Theme.small
        wrapMode: Text.Wrap
        width: rp.width
    }
    Repeater {
        model: rp.params
        RowLayout {
            id: row
            required property var modelData
            readonly property var parts: modelData.label.split("/")
            readonly property bool ip: modelData.key.endsWith("TargetIP")
            width: rp.width
            spacing: 4
            Label {
                text: row.parts.slice(-2).join(" › ")
                color: Theme.muted
                font.pixelSize: Theme.small
                elide: Text.ElideLeft
                Layout.fillWidth: true
                ToolTip.visible: hover.hovered
                ToolTip.text: row.modelData.label + (row.modelData.min !== undefined
                              ? qsTr(" (%1 … %2)").arg(row.modelData.min).arg(row.modelData.max) : "")
                HoverHandler { id: hover }
            }
            Check {
                visible: row.modelData.type === "BOOL"
                checked: row.modelData.value === "1"
                onToggled: rp.commit(row.modelData.key, checked)
            }
            Input {
                visible: row.modelData.type !== "BOOL"
                Layout.preferredWidth: 110
                implicitHeight: 22
                font.family: Theme.mono
                font.pixelSize: Theme.small
                text: row.ip && row.modelData.value === "TT_DISABLED" ? "" : row.modelData.value
                placeholderText: row.ip ? qsTr("Arena default") : row.modelData.def
                onEditingFinished: if (text !== row.modelData.value) rp.commit(row.modelData.key, text)
            }
        }
    }
}
