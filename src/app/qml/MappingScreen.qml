import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Mapping screen (FR-15, B3): the composition with every wall slice and strip as a rotatable quad, grouped by
// output. Drag or arrow keys move the selected slice in composition pixels (Shift: 10 px); each move is one edit.
// The left panel edits the composition, the outputs and their Arena params.
Rectangle {
    id: screen
    color: Theme.bg
    property string output // selected output uuid
    property var hidden: ({}) // output uuid -> true, view state only
    property bool fit: true
    property int reload: 0
    readonly property var comp: Session.compositionInfo
    readonly property real compW: comp.width || 1920
    readonly property real compH: comp.height || 1080
    readonly property real zoom: fit ? Math.min((canvas.width - 24) / compW, (canvas.height - 24) / compH) : 1
    readonly property var selectedOutput: Session.outputs.find(o => o.uuid === output) || null

    function fmt(v) { return v === undefined || v === null ? "" : String(Math.round(Number(v) * 1000) / 1000) }
    function nudge(dx, dy) {
        const o = Session.current
        if (!o.slice) return
        Session.edit(o.uuid, "/slice", Object.assign({}, o.slice, { x: o.slice.x + dx, y: o.slice.y + dy }))
    }

    Connections {
        target: Session
        function onChanged() { screen.reload++ }
    }
    // shortcut: a live source refreshes the preview at 4 Hz through the image provider; sample the render thread's
    // texture in a QQuickRhiItem when mapping needs full-rate video.
    Timer {
        interval: 250
        repeat: true
        running: screen.visible && Session.mediaStatus.live
        onTriggered: screen.reload++
    }
    readonly property bool typing: Window.activeFocusItem instanceof TextInput
    Shortcut { sequence: "Left"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(-1, 0) }
    Shortcut { sequence: "Right"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(1, 0) }
    Shortcut { sequence: "Up"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(0, -1) }
    Shortcut { sequence: "Down"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(0, 1) }
    Shortcut { sequence: "Shift+Left"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(-10, 0) }
    Shortcut { sequence: "Shift+Right"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(10, 0) }
    Shortcut { sequence: "Shift+Up"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(0, -10) }
    Shortcut { sequence: "Shift+Down"; enabled: screen.visible && !screen.typing; onActivated: screen.nudge(0, 10) }

    component Section: Label {
        color: Theme.muted
        font.pixelSize: Theme.small
        font.letterSpacing: 1
        topPadding: 8
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            color: Theme.panel
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
            ScrollView {
                anchors { fill: parent; margins: 12 }
                contentWidth: availableWidth
                Column {
                    width: parent.width
                    spacing: 4

                    Section { text: qsTr("COMPOSITION") }
                    Field {
                        label: qsTr("Size px")
                        values: [screen.fmt(screen.compW), screen.fmt(screen.compH)]
                        onCommitted: (i, t) => Number(t) >= 16 ? Session.edit("composition", i ? "/height" : "/width", Number(t))
                                                               : Session.changed()
                    }
                    ResolumeParams { uuid: "composition" }

                    Section { text: qsTr("OUTPUTS") }
                    Label {
                        visible: Session.outputs.length === 0
                        text: qsTr("Import a Resolume Advanced Output on the Project screen, or export once to create them.")
                        color: Theme.muted
                        font.pixelSize: Theme.small + 1
                        wrapMode: Text.Wrap
                        width: parent.width
                    }
                    Repeater {
                        model: Session.outputs
                        Rectangle {
                            id: outRow
                            required property var modelData
                            width: parent.width
                            height: 24
                            radius: 3
                            color: screen.output === modelData.uuid ? Theme.accentSoft : "transparent"
                            RowLayout {
                                anchors { fill: parent; leftMargin: 2; rightMargin: 4 }
                                spacing: 4
                                Check {
                                    checked: !screen.hidden[outRow.modelData.uuid]
                                    onToggled: {
                                        const h = Object.assign({}, screen.hidden)
                                        h[outRow.modelData.uuid] = !checked
                                        screen.hidden = h
                                    }
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Show this output's slices")
                                }
                                Label {
                                    text: outRow.modelData.name
                                    color: Theme.fg
                                    font.pixelSize: Theme.small + 1
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: outRow.modelData.kind === "lumiverse"
                                          ? qsTr("Lumiverse · U%1").arg(outRow.modelData.universe)
                                          : outRow.modelData.kind + " · " + outRow.modelData.width + "×" + outRow.modelData.height
                                    color: Theme.muted
                                    font.family: Theme.mono
                                    font.pixelSize: Theme.small
                                }
                            }
                            TapHandler { onTapped: screen.output = outRow.modelData.uuid }
                        }
                    }

                    Column {
                        visible: !!screen.selectedOutput
                        width: parent.width
                        spacing: 4
                        readonly property var o: screen.selectedOutput || {}
                        Section { text: qsTr("OUTPUT") }
                        Field {
                            label: qsTr("Name")
                            numeric: false
                            values: [parent.o.name || ""]
                            onCommitted: (i, t) => Session.edit(screen.output, "/name", t)
                        }
                        Field {
                            visible: parent.o.kind !== "lumiverse"
                            label: qsTr("Size px")
                            values: [screen.fmt(parent.o.width), screen.fmt(parent.o.height)]
                            onCommitted: (i, t) => Number(t) >= 1 ? Session.edit(screen.output, i ? "/height" : "/width", Number(t))
                                                                  : Session.changed()
                        }
                        Field {
                            visible: parent.o.kind === "lumiverse"
                            label: qsTr("Universe")
                            values: [screen.fmt(parent.o.universe)]
                            // Project universe of the Lumiverse's first channel; its strips' patches follow on export.
                            onCommitted: (i, t) => Number(t) >= 1 ? Session.edit(screen.output, "/universe", Number(t))
                                                                  : Session.changed()
                        }
                        ResolumeParams { uuid: screen.output; open: true }
                    }

                    Section { text: qsTr("VIEW") }
                    RowLayout {
                        spacing: 6
                        Button { text: qsTr("Fit"); checkable: true; checked: screen.fit; onClicked: screen.fit = true }
                        Button { text: "1:1"; checkable: true; checked: !screen.fit; onClicked: screen.fit = false }
                    }
                    Repeater {
                        model: Session.resolumeWarnings
                        Label {
                            required property string modelData
                            text: "⚠ " + modelData
                            color: Theme.warn
                            font.pixelSize: Theme.small
                            wrapMode: Text.Wrap
                            width: parent.width
                        }
                    }
                }
            }
        }

        Flickable {
            id: canvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: Math.max(width, screen.compW * screen.zoom + 24)
            contentHeight: Math.max(height, screen.compH * screen.zoom + 24)
            interactive: !screen.fit

            TapHandler { onTapped: Session.selectUuid("", false) }

            Item {
                id: page
                x: Math.max(12, (canvas.width - width) / 2)
                y: Math.max(12, (canvas.height - height) / 2)
                width: screen.compW * screen.zoom
                height: screen.compH * screen.zoom

                Image {
                    anchors.fill: parent
                    source: "image://composition/" + screen.reload
                    cache: false
                    smooth: false
                }
                Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.line }

                Repeater {
                    model: Session.objects.filter(o => o.slice && (o.type === "ledWall" || o.type === "pixelStrip")
                                                      && !screen.hidden[o.output])
                    Rectangle {
                        id: quad
                        required property var modelData
                        readonly property var s: modelData.slice
                        readonly property bool strip: modelData.type === "pixelStrip"
                        x: s.x * screen.zoom
                        y: s.y * screen.zoom
                        width: Math.max(2, s.w * screen.zoom)
                        height: Math.max(2, s.h * screen.zoom)
                        rotation: s.angle || 0
                        color: modelData.selected ? Qt.alpha(Theme.accent, 0.25) : "transparent"
                        border.width: modelData.selected ? 2 : 1
                        border.color: modelData.selected ? Theme.accent : strip ? Theme.ok : Theme.fg
                        Label {
                            anchors.centerIn: parent
                            visible: !quad.strip || quad.modelData.selected
                            text: quad.modelData.name
                            color: Theme.fg
                            font.pixelSize: Theme.small
                            style: Text.Outline
                            styleColor: Theme.viewport
                        }
                        TapHandler {
                            onTapped: (pt, button) => Session.selectUuid(quad.modelData.uuid, pt.modifiers & Qt.ControlModifier)
                        }
                        DragHandler {
                            id: drag
                            onActiveChanged: {
                                if (!active)
                                    Session.edit(quad.modelData.uuid, "/slice", Object.assign({}, quad.s, {
                                        x: Math.round(quad.x / screen.zoom), y: Math.round(quad.y / screen.zoom) }))
                                else if (!quad.modelData.selected)
                                    Session.selectUuid(quad.modelData.uuid, false)
                            }
                        }
                    }
                }
            }
        }
    }
}
