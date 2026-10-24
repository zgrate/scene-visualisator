import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// Light control: pick groups or fixtures on the left (they become the selection), set them on the right. Touched
// attributes are held in the programmer and override live DMX until released.
Rectangle {
    id: ctl
    color: Theme.bg
    readonly property var sel: Session.selectedUuids
    readonly property var groups: Session.controlGroups
    readonly property var held: Session.programmer
    readonly property int heldCount: Object.keys(held).length
    readonly property var attrs: Session.controlAttributes(sel)
    readonly property int heads: Session.controlHeads(sel)
    property int head: -1 // the head the faders act on, -1 all
    onHeadsChanged: if (head >= heads) head = -1
    readonly property var first: {
        const whole = sel.length ? (held[sel[0]] || ({})) : ({})
        return head < 0 ? whole : Object.assign({}, whole, (whole.heads || {})[head] || {})
    }
    readonly property var fixtures: {
        const ids = new Set()
        groups.forEach(g => { if (g.kind === "type") g.uuids.forEach(u => ids.add(u)) })
        return Session.patchRows.filter(r => ids.has(r.uuid))
    }

    function has(a) { return attrs.indexOf(a) >= 0 }
    function value(a, fallback) { return first[a] !== undefined ? first[a] : fallback }
    function set(map) { if (sel.length) Session.setLight(sel, map, head) }
    function colour(r, g, b) {
        set({ ColorAdd_R: r, ColorAdd_G: g, ColorAdd_B: b, ColorAdd_W: r === 1 && g === 1 && b === 1 ? 1 : 0,
              ColorSub_C: 1 - r, ColorSub_M: 1 - g, ColorSub_Y: 1 - b })
    }
    function pick(uuids, add) {
        Session.selectUuids(add ? Array.from(new Set(sel.concat(uuids))) : uuids)
    }

    component Header: Label {
        color: Theme.muted
        font.pixelSize: Theme.small
        font.letterSpacing: 1
    }
    component Panel: Rectangle {
        color: Theme.panel
        border.color: Theme.line
        radius: Theme.radius
    }
    // A fader for one attribute; it follows the first selected fixture's held value unless being dragged.
    component Fader: RowLayout {
        id: f
        required property string label
        required property string attribute
        property real from: 0
        property real to: 1
        property real rest: 0
        property string unit: "%"
        enabled: ctl.has(attribute)
        visible: enabled || !ctl.sel.length // only what the selection has
        spacing: 8
        Label { text: f.label; color: Theme.fg; font.pixelSize: Theme.fontSize; Layout.preferredWidth: 70 }
        Slider {
            id: s
            Layout.fillWidth: true
            from: f.from
            to: f.to
            onMoved: ctl.set({ [f.attribute]: value })
            Binding on value { value: ctl.value(f.attribute, f.rest); when: !s.pressed }
        }
        Label {
            text: f.unit === "%" ? Math.round(s.value * 100) + " %" : s.value.toFixed(0) + " " + f.unit
            color: ctl.first[f.attribute] !== undefined ? Theme.warn : Theme.muted
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
            Layout.preferredWidth: 56
            horizontalAlignment: Text.AlignRight
        }
        Button { // drop just this attribute back to live input
            text: "×"
            flat: true
            enabled: ctl.first[f.attribute] !== undefined
            onClicked: ctl.set({ [f.attribute]: null })
            Layout.preferredWidth: 28
        }
    }

    RowLayout {
        anchors { fill: parent; margins: 16 }
        spacing: 12

        // Targets: groups, then fixtures
        Panel {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            ColumnLayout {
                anchors { fill: parent; margins: 8 }
                spacing: 6
                Header { text: qsTr("GROUPS") }
                ListView {
                    id: groupList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 1
                    clip: true
                    model: ctl.groups
                    section.property: "kind"
                    section.delegate: Header {
                        required property string section
                        topPadding: 6
                        text: ({ saved: qsTr("SAVED"), rig: qsTr("RIG"), type: qsTr("FIXTURE TYPES") })[section]
                    }
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        readonly property bool all: modelData.uuids.length > 0
                                                    && modelData.uuids.every(u => ctl.sel.indexOf(u) >= 0)
                        width: ListView.view.width
                        height: 24
                        radius: 3
                        color: all ? Theme.accentSoft : hover.hovered ? Theme.line : "transparent"
                        HoverHandler { id: hover }
                        TapHandler {
                            onTapped: (point, button) => ctl.pick(row.modelData.uuids,
                                                                  point.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                        }
                        RowLayout {
                            anchors { fill: parent; leftMargin: 8; rightMargin: 4 }
                            Label {
                                text: row.modelData.name
                                color: Theme.fg
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fontSize
                                Layout.fillWidth: true
                            }
                            Label {
                                text: row.modelData.uuids.length
                                color: Theme.muted
                                font.family: Theme.mono
                                font.pixelSize: Theme.small + 1
                            }
                            Button {
                                visible: row.modelData.kind === "saved"
                                text: "×"
                                flat: true
                                Layout.preferredWidth: 22
                                Layout.preferredHeight: 20
                                onClicked: Session.deleteGroup(row.modelData.key)
                            }
                        }
                    }
                }
                RowLayout {
                    Input {
                        id: groupName
                        Layout.fillWidth: true
                        placeholderText: qsTr("New group from selection")
                        onAccepted: saveGroup.clicked()
                    }
                    Button {
                        id: saveGroup
                        text: qsTr("Save")
                        enabled: ctl.sel.length > 0 && groupName.text.trim().length > 0
                        onClicked: { if (!Session.saveGroup(groupName.text, ctl.sel)) groupName.text = "" }
                    }
                }
                Header { text: qsTr("LOOKS"); topPadding: 6 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: Session.looks
                        delegate: Row {
                            id: look
                            required property var modelData
                            Button {
                                text: look.modelData.name
                                onClicked: Session.recallLook(look.modelData.uuid)
                            }
                            Button {
                                text: "×"
                                flat: true
                                width: 22
                                onClicked: Session.deleteLook(look.modelData.uuid)
                            }
                        }
                    }
                }
                RowLayout {
                    Input {
                        id: lookName
                        Layout.fillWidth: true
                        placeholderText: qsTr("New look from what is held")
                        onAccepted: saveLook.clicked()
                    }
                    Button {
                        id: saveLook
                        text: qsTr("Save")
                        enabled: Object.keys(Session.programmer).length > 0 && lookName.text.trim().length > 0
                        onClicked: { if (!Session.saveLook(lookName.text)) lookName.text = "" }
                    }
                }
                Header { text: qsTr("FIXTURES · %1 SELECTED").arg(ctl.sel.length); topPadding: 6 }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 1
                    clip: true
                    model: ctl.fixtures
                    delegate: Rectangle {
                        id: fx
                        required property var modelData
                        width: ListView.view.width
                        height: 22
                        radius: 3
                        color: modelData.selected ? Theme.accentSoft : fxHover.hovered ? Theme.line : "transparent"
                        HoverHandler { id: fxHover }
                        TapHandler {
                            onTapped: (point, button) => Session.selectUuid(fx.modelData.uuid,
                                                                            point.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                        }
                        RowLayout {
                            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                            Label {
                                text: fx.modelData.fixtureId !== undefined ? fx.modelData.fixtureId : "—"
                                color: Theme.muted
                                font.family: Theme.mono
                                font.pixelSize: Theme.small + 1
                                Layout.preferredWidth: 32
                            }
                            Label {
                                text: fx.modelData.name
                                color: Theme.fg
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fontSize
                                Layout.fillWidth: true
                            }
                            Label { // held in the programmer
                                text: "●"
                                visible: ctl.held[fx.modelData.uuid] !== undefined
                                color: Theme.warn
                                font.pixelSize: Theme.small
                            }
                        }
                    }
                }
            }
        }

        // Controls for the selection
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            RowLayout {
                spacing: 8
                Label {
                    text: ctl.sel.length ? qsTr("%n fixture(s) selected", "", ctl.sel.length) : qsTr("Select a group or fixtures")
                    color: Theme.fg
                    font.bold: true
                    font.pixelSize: 14
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("PROGRAMMER · %1 HELD").arg(ctl.heldCount)
                    color: ctl.heldCount ? Theme.warn : Theme.muted
                    font.pixelSize: Theme.small
                    font.letterSpacing: 1
                }
            }
            RowLayout {
                spacing: 6
                enabled: ctl.sel.length > 0
                Button { text: qsTr("Full"); onClicked: ctl.set({ Dimmer: 1 }) }
                Button { text: qsTr("Out"); onClicked: ctl.set({ Dimmer: 0 }) }
                Button {
                    readonly property bool on: ctl.sel.some(u => Session.located.includes(u))
                    text: on ? qsTr("Unlocate") : qsTr("Locate")
                    onClicked: Session.locate(ctl.sel, !on)
                }
                Button { text: qsTr("Release selection"); onClicked: Session.releaseLights(ctl.sel) }
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Release all")
                    enabled: ctl.heldCount > 0
                    palette.buttonText: Theme.warn
                    onClicked: Session.releaseLights([])
                }
            }

            RowLayout { // multi-head fixtures: which head the faders below act on
                visible: ctl.heads >= 2
                spacing: 4
                Header { text: qsTr("HEAD") }
                Repeater {
                    model: ctl.heads + 1
                    delegate: Button {
                        required property int index
                        text: index === 0 ? qsTr("All") : index
                        highlighted: ctl.head === index - 1
                        onClicked: ctl.head = index - 1
                    }
                }
            }

            Panel {
                Layout.fillWidth: true
                implicitHeight: intensity.implicitHeight + 24
                ColumnLayout {
                    id: intensity
                    anchors { fill: parent; margins: 12 }
                    Header { text: qsTr("INTENSITY") }
                    Fader { label: qsTr("Dimmer"); attribute: "Dimmer" }
                    RowLayout {
                        enabled: ctl.has("Shutter1Strobe")
                        Check {
                            text: qsTr("Strobe")
                            checked: ctl.first.Shutter1Strobe !== undefined
                            onToggled: ctl.set({ Shutter1Strobe: checked ? strobe.value : null })
                        }
                        Slider {
                            id: strobe
                            Layout.fillWidth: true
                            from: 1
                            to: 25
                            onMoved: ctl.set({ Shutter1Strobe: value })
                            Binding on value { value: ctl.value("Shutter1Strobe", 5); when: !strobe.pressed }
                        }
                        Label {
                            text: strobe.value.toFixed(1) + " Hz"
                            color: Theme.muted
                            font.family: Theme.mono
                            font.pixelSize: Theme.small + 1
                            Layout.preferredWidth: 56
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            Panel {
                Layout.fillWidth: true
                implicitHeight: colourCol.implicitHeight + 24
                ColumnLayout {
                    id: colourCol
                    anchors { fill: parent; margins: 12 }
                    enabled: ["ColorAdd_R", "ColorSub_C", "ColorAdd_W"].some(a => ctl.has(a))
                    Header { text: qsTr("COLOUR") }
                    RowLayout {
                        spacing: 6
                        // Light colours are show data, not app chrome, so they are not Theme tokens.
                        Repeater {
                            model: [[1, 1, 1], [1, 0, 0], [1, 0.5, 0], [1, 1, 0], [0, 1, 0], [0, 1, 1], [0, 0, 1], [1, 0, 1]]
                            Rectangle {
                                required property var modelData
                                width: 32
                                height: 24
                                radius: 4
                                color: Qt.rgba(modelData[0], modelData[1], modelData[2], 1)
                                border.color: swatchHover.hovered ? Theme.accent : Theme.line
                                HoverHandler { id: swatchHover }
                                TapHandler { onTapped: ctl.colour(modelData[0], modelData[1], modelData[2]) }
                            }
                        }
                    }
                    Fader { label: qsTr("Red"); attribute: "ColorAdd_R" }
                    Fader { label: qsTr("Green"); attribute: "ColorAdd_G" }
                    Fader { label: qsTr("Blue"); attribute: "ColorAdd_B" }
                    Fader { label: qsTr("White"); attribute: "ColorAdd_W" }
                    Fader { label: qsTr("Cyan"); attribute: "ColorSub_C" }
                    Fader { label: qsTr("Magenta"); attribute: "ColorSub_M" }
                    Fader { label: qsTr("Yellow"); attribute: "ColorSub_Y" }
                }
            }

            Panel {
                Layout.fillWidth: true
                implicitHeight: beamCol.implicitHeight + 24
                ColumnLayout {
                    id: beamCol
                    anchors { fill: parent; margins: 12 }
                    Header { text: qsTr("POSITION AND BEAM") }
                    Fader { label: qsTr("Pan"); attribute: "Pan"; from: -270; to: 270; unit: "°" }
                    Fader { label: qsTr("Tilt"); attribute: "Tilt"; from: -135; to: 135; unit: "°" }
                    Fader { label: qsTr("Zoom"); attribute: "Zoom"; from: 2; to: 60; rest: 20; unit: "°" }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }
}
