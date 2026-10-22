import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Fields of the first selected object. Every change is one undoable Session edit.
Column {
    id: insp
    readonly property var o: Session.current
    readonly property string uuid: o.uuid || ""
    width: parent ? parent.width : 0
    spacing: 4

    function fmt(v) { return v === undefined || v === null ? "" : String(Math.round(Number(v) * 1000) / 1000) }
    function num(pointer, text) {
        const v = Number(text)
        if (text.trim() !== "" && isFinite(v)) Session.edit(uuid, pointer, v)
        else Session.changed() // invalid input: re-read the stored value
    }

    Label {
        visible: !insp.uuid
        text: qsTr("Nothing selected")
        color: Theme.muted
        font.pixelSize: Theme.fontSize
    }
    Label {
        visible: Session.selectionCount > 1
        text: qsTr("%1 objects selected · showing the first").arg(Session.selectionCount)
        color: Theme.muted
        font.pixelSize: Theme.small
    }

    Column {
        visible: !!insp.uuid
        width: parent.width
        spacing: 4

        Field {
            label: qsTr("Name")
            numeric: false
            values: [insp.o.name || ""]
            onCommitted: (i, t) => Session.edit(insp.uuid, "/name", t)
        }
        Field {
            label: qsTr("Position")
            values: (insp.o.position || [0, 0, 0]).map(insp.fmt)
            onCommitted: (i, t) => insp.num("/position/" + i, t)
        }
        Field {
            label: qsTr("Rotation")
            values: (insp.o.rotation || [0, 0, 0]).map(insp.fmt)
            onCommitted: (i, t) => insp.num("/rotation/" + i, t)
        }

        // Fixture
        Column {
            visible: insp.o.type === "fixture"
            width: parent.width
            spacing: 4
            RowLayout {
                width: parent.width
                Label { text: qsTr("Type"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: Session.fixtureTypes
                    currentIndex: Session.fixtureTypes.indexOf(insp.o.gdtf || "")
                    displayText: insp.o.typeName || insp.o.gdtf || ""
                    onActivated: i => Session.setFixtureType(insp.uuid, Session.fixtureTypes[i])
                }
            }
            RowLayout {
                width: parent.width
                Label { text: qsTr("Mode"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    id: modeBox
                    readonly property var modes: insp.o.type === "fixture" ? Session.modes(insp.o.gdtf) : []
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: modes
                    currentIndex: modes.indexOf(insp.o.mode || "")
                    onActivated: i => Session.edit(insp.uuid, "/mode", modes[i])
                }
            }
            Field {
                label: qsTr("Fixture ID")
                values: [insp.fmt(insp.o.fixtureId)]
                onCommitted: (i, t) => t.trim() === "" ? Session.edit(insp.uuid, "/fixtureId", null)
                                                       : insp.num("/fixtureId", t)
            }
            Field {
                label: qsTr("Patch")
                values: [insp.o.patchText || ""]
                warn: !!insp.o.overflow
                onCommitted: (i, t) => { if (!Session.setPatch(insp.uuid, t)) Session.changed() }
            }
            Label {
                x: 66
                text: insp.o.overflow ? qsTr("%1 channels · runs past 512").arg(insp.o.footprint)
                                      : qsTr("%1 channels").arg(insp.o.footprint)
                color: insp.o.overflow ? Theme.warn : Theme.muted
                font.pixelSize: Theme.small
            }
        }

        // LED wall
        Column {
            visible: insp.o.type === "ledWall"
            width: parent.width
            spacing: 4
            Field {
                label: qsTr("Size m")
                values: [insp.fmt(insp.o.width), insp.fmt(insp.o.height)]
                onCommitted: (i, t) => insp.num(i ? "/height" : "/width", t)
            }
            Field {
                label: qsTr("Pitch mm")
                values: [insp.fmt(insp.o.pixelPitch)]
                onCommitted: (i, t) => insp.num("/pixelPitch", t)
            }
            Field {
                label: qsTr("Module m")
                values: [insp.fmt(insp.o.moduleWidth), insp.fmt(insp.o.moduleHeight)]
                onCommitted: (i, t) => insp.num(i ? "/moduleHeight" : "/moduleWidth", t)
            }
            Field {
                label: qsTr("Nits · γ")
                values: [insp.fmt(insp.o.nits), insp.fmt(insp.o.gamma)]
                onCommitted: (i, t) => insp.num(i ? "/gamma" : "/nits", t)
            }
            Field {
                label: qsTr("Slice")
                readonly property var s: insp.o.slice || {}
                values: [insp.fmt(s.x), insp.fmt(s.y), insp.fmt(s.w), insp.fmt(s.h)]
                onCommitted: (i, t) => insp.num("/slice/" + ["x", "y", "w", "h"][i], t)
            }
            Label {
                x: 66
                text: insp.o.resolution || ""
                color: Theme.muted
                font.pixelSize: Theme.small
            }
        }

        // Truss
        Column {
            visible: insp.o.type === "truss"
            width: parent.width
            spacing: 4
            RowLayout {
                width: parent.width
                Label { text: qsTr("Type"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    readonly property var types: ["straight", "corner", "circle"]
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: types
                    currentIndex: types.indexOf(insp.o.trussType || "straight")
                    onActivated: i => Session.edit(insp.uuid, "/trussType", types[i])
                }
            }
            Field {
                label: qsTr("Length m")
                values: [insp.fmt(insp.o.length)]
                onCommitted: (i, t) => insp.num("/length", t)
            }
        }

        RowLayout {
            width: parent.width
            spacing: 6
            Button {
                text: qsTr("Duplicate")
                font.pixelSize: Theme.small + 1
                onClicked: Session.duplicateSelection()
            }
            Button {
                text: qsTr("Delete")
                font.pixelSize: Theme.small + 1
                onClicked: Session.deleteSelection()
            }
        }
    }
}
