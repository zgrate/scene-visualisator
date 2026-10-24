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

    // Composition rect in pixels plus rotation (Arena InputRect).
    component SliceField: Field {
        label: qsTr("Slice")
        readonly property var s: insp.o.slice || {}
        values: [insp.fmt(s.x), insp.fmt(s.y), insp.fmt(s.w), insp.fmt(s.h), insp.fmt(s.angle || 0)]
        onCommitted: (i, t) => insp.num("/slice/" + ["x", "y", "w", "h", "angle"][i], t)
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
        RowLayout {
            width: parent.width
            Label { text: qsTr("Layer"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
            ComboBox {
                readonly property var layers: [{ uuid: null, name: qsTr("(none)") }].concat(Session.layers)
                Layout.fillWidth: true
                implicitHeight: 24
                font.pixelSize: Theme.small + 1
                model: layers.map(l => l.name)
                currentIndex: Math.max(0, layers.findIndex(l => l.uuid === (insp.o.layer || null)))
                // Applies to the whole selection.
                onActivated: i => Session.editSelection("/layer", layers[i].uuid)
            }
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
        Field {
            label: qsTr("Scale")
            values: (insp.o.scale || [1, 1, 1]).map(insp.fmt)
            onCommitted: (i, t) => insp.num("/scale/" + i, t)
        }

        // Deck: bottom sits on its position
        Field {
            visible: insp.o.type === "deck"
            label: qsTr("W D H m")
            values: [insp.fmt(insp.o.width), insp.fmt(insp.o.depth), insp.fmt(insp.o.height)]
            onCommitted: (i, t) => insp.num("/" + ["width", "depth", "height"][i], t)
        }

        // Shape primitive
        Column {
            visible: insp.o.type === "shape"
            width: parent.width
            spacing: 4
            RowLayout {
                width: parent.width
                Label { text: qsTr("Shape"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    readonly property var kinds: ["box", "cylinder", "wedge"]
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: kinds
                    currentIndex: kinds.indexOf(insp.o.shape || "box")
                    onActivated: i => Session.edit(insp.uuid, "/shape", kinds[i])
                }
            }
            Field {
                label: qsTr("Size m")
                values: (insp.o.size || [1, 1, 1]).map(insp.fmt)
                onCommitted: (i, t) => insp.num("/size/" + i, t)
            }
            Field {
                label: qsTr("Colour")
                numeric: false
                values: [insp.o.color || ""]
                onCommitted: (i, t) => { if (/^#[0-9a-fA-F]{6}$/.test(t)) Session.editSelection("/color", t, "shape"); else Session.changed() }
            }
            Row {
                x: 66
                spacing: 4
                // Material presets stored in the document (floor, wall, light, wood, black, white), not app chrome.
                Repeater {
                    model: ["#2a2d33", "#8a8f99", "#c9ccd2", "#7a5a3c", "#15171b", "#e8e6e1"]
                    Rectangle {
                        required property string modelData
                        width: 18; height: 18; radius: 3
                        color: modelData
                        border.color: insp.o.color === modelData ? Theme.accent : Theme.line
                        TapHandler { onTapped: Session.editSelection("/color", parent.modelData, "shape") }
                    }
                }
            }
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
            RowLayout {
                visible: (insp.o.strings || []).length > 0
                width: parent.width
                Label { text: qsTr("Facing"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    readonly property var facings: ["all", "down", "up", "front", "back", "left", "right"]
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: [qsTr("All round"), qsTr("Down"), qsTr("Up"), qsTr("Front (audience)"), qsTr("Back"),
                        qsTr("Left (−X)"), qsTr("Right (+X)")]
                    currentIndex: Math.max(0, facings.indexOf(insp.o.facing || "all"))
                    onActivated: i => Session.editSelection("/facing", facings[i], "ledWall")
                }
            }
            SliceField {}
            Label {
                x: 66
                text: insp.o.resolution || ""
                color: Theme.muted
                font.pixelSize: Theme.small
            }
        }

        // Pixel strip (FR-16)
        Column {
            visible: insp.o.type === "pixelStrip"
            width: parent.width
            spacing: 4
            Field {
                label: qsTr("Pixels")
                values: [insp.fmt(insp.o.pixels), insp.fmt(insp.o.rows)]
                onCommitted: (i, t) => Number(t) >= 1 ? insp.num(i ? "/rows" : "/pixels", t) : Session.changed()
            }
            Field {
                label: qsTr("Length m")
                values: [insp.fmt(insp.o.length)]
                onCommitted: (i, t) => insp.num("/length", t)
            }
            RowLayout {
                width: parent.width
                Label { text: qsTr("Colour"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    readonly property var formats: ["rgb", "rbg", "grb", "gbr", "brg", "bgr", "rgbw", "grbw", "w"]
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: formats.map(f => f.toUpperCase())
                    currentIndex: formats.indexOf(insp.o.colorFormat || "rgb")
                    onActivated: i => Session.edit(insp.uuid, "/colorFormat", formats[i])
                }
            }
            Field {
                label: qsTr("Dist · γ")
                values: [insp.fmt(insp.o.distribution), insp.fmt(insp.o.gamma)]
                // Distribution: pixels per universe, at most 512 / bytes per pixel (170 RGB, 128 RGBW).
                onCommitted: (i, t) => {
                    if (i === 1) return insp.num("/gamma", t)
                    const max = Math.floor(512 / Math.max(1, (insp.o.colorFormat || "rgb").length))
                    Number(t) >= 1 && Number(t) <= max ? insp.num("/distribution", t) : Session.changed()
                }
            }
            Field {
                label: qsTr("Patch")
                values: [insp.o.patchText || ""]
                onCommitted: (i, t) => { if (!Session.setPatch(insp.uuid, t)) Session.changed() }
            }
            Label {
                x: 66
                text: insp.o.lastText ? qsTr("%1 channels · to %2").arg(insp.o.footprint).arg(insp.o.lastText)
                                      : qsTr("%1 channels").arg(insp.o.footprint || 0)
                color: Theme.muted
                font.pixelSize: Theme.small
            }
            RowLayout {
                width: parent.width
                Label { text: qsTr("Source"); color: Theme.muted; font.pixelSize: Theme.small + 1; Layout.preferredWidth: 62 }
                ComboBox {
                    readonly property var sources: ["dmx", "slice"]
                    Layout.fillWidth: true
                    implicitHeight: 24
                    font.pixelSize: Theme.small + 1
                    model: [qsTr("DMX (as WLED shows it)"), qsTr("Composition slice")]
                    currentIndex: sources.indexOf(insp.o.pixelSource || "dmx")
                    onActivated: i => Session.editSelection("/pixelSource", sources[i], "pixelStrip") // every selected strip
                }
            }
            SliceField {}
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

        ResolumeParams {
            visible: insp.o.type === "ledWall" || insp.o.type === "pixelStrip"
            uuid: insp.uuid
        }

        RowLayout {
            width: parent.width
            spacing: 6
            Button {
                visible: insp.o.type === "fixture"
                readonly property bool on: Session.located.includes(insp.uuid)
                text: on ? qsTr("Unlocate") : qsTr("Locate")
                font.pixelSize: Theme.small + 1
                onClicked: Session.locate(Session.selectedUuids, !on)
            }
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
