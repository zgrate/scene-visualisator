import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// Patch sheet: every fixture in one sortable, filterable list. Click selects (Ctrl/Cmd toggles), the patch cell edits
// in place, and "Re-address" packs the selection from a start address in fixture-ID order.
Rectangle {
    id: sheet
    color: Theme.bg
    property string sortKey: "fixtureId"
    property bool ascending: true
    property string filter: ""
    property string message
    property int meterUniverse: Session.current.patch ? Session.current.patch.universe
                              : (Session.universes.length ? Session.universes[0] : 1)

    readonly property var columns: [["fixtureId", qsTr("ID"), 50], ["name", qsTr("Name"), -1], ["type", qsTr("Type"), 170],
                                    ["mode", qsTr("Mode"), 110], ["patch", qsTr("Patch"), 80],
                                    ["footprint", qsTr("Ch"), 40], ["position", qsTr("Position (m)"), 150]]

    function key(r, k) {
        if (k === "patch") return r.universe === undefined ? Infinity : r.universe * 1000 + r.address
        if (k === "fixtureId") return r.fixtureId === undefined ? Infinity : r.fixtureId
        if (k === "position") return r.position[0]
        return String(r[k]).toLowerCase()
    }
    readonly property var rows: {
        const f = filter.toLowerCase()
        const out = Session.patchRows.filter(r => !f || [r.fixtureId, r.name, r.type, r.mode, r.patch]
                                                   .some(v => String(v ?? "").toLowerCase().includes(f)))
        const s = ascending ? 1 : -1
        return out.sort((a, b) => { const x = key(a, sortKey), y = key(b, sortKey); return x < y ? -s : x > y ? s : 0 })
    }
    // Name takes what the fixed columns and the 24 px flag leave over.
    function colWidth(i) {
        const w = columns[i][2]
        return w >= 0 ? w : head.width - columns.reduce((t, c) => t + Math.max(c[2], 0), 0) - 8 * columns.length - 24
    }
    function fmt(v) { return v.map(n => n.toFixed(2)).join("  ") }

    ColumnLayout {
        anchors { fill: parent; margins: 16 }
        spacing: 10

        RowLayout {
            spacing: 10
            Label { text: qsTr("PATCH"); color: Theme.muted; font.pixelSize: Theme.small; font.letterSpacing: 1 }
            Label {
                textFormat: Text.StyledText
                text: qsTr("%1 fixtures").arg(Session.patchRows.length)
                      + (Session.conflictCount ? " · <font color='" + Theme.err + "'>"
                                                 + qsTr("%1 in conflict").arg(Session.conflictCount) + "</font>" : "")
                color: Theme.fg
                font.pixelSize: Theme.fontSize
            }
            Input {
                placeholderText: qsTr("Filter")
                implicitWidth: 180
                onTextChanged: sheet.filter = text
            }
            Item { Layout.fillWidth: true }
            HudButton {
                text: qsTr("Select shown")
                onClicked: Session.selectUuids(sheet.rows.map(r => r.uuid))
            }
            Label { text: qsTr("Re-address %1 from").arg(Session.selectionCount); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            Input {
                id: start
                placeholderText: "1.1"
                implicitWidth: 70
                font.family: Theme.mono
                onAccepted: go.clicked()
            }
            HudButton {
                id: go
                text: qsTr("Re-address")
                enabled: Session.selectionCount > 0 && start.text !== ""
                onClicked: sheet.message = Session.readdressSelection(start.text)
            }
            HudButton {
                readonly property bool on: Session.selectedUuids.some(u => Session.located.includes(u))
                text: qsTr("Locate")
                active: on
                enabled: Session.selectionCount > 0
                onClicked: Session.locate(Session.selectedUuids, !on)
            }
        }
        Label {
            visible: sheet.message !== ""
            text: sheet.message
            color: Theme.err
            font.pixelSize: Theme.small + 1
        }

        // Occupancy of one universe: used, conflicting and selected channels.
        RowLayout {
            spacing: 10
            Label { text: qsTr("UNIVERSE"); color: Theme.muted; font.pixelSize: Theme.small; font.letterSpacing: 1 }
            ComboBox {
                implicitWidth: 64
                implicitHeight: 24
                font.pixelSize: Theme.small + 1
                model: Session.universes
                currentIndex: Session.universes.indexOf(sheet.meterUniverse)
                onActivated: i => sheet.meterUniverse = Session.universes[i]
            }
            Rectangle {
                Layout.fillWidth: true
                height: 30
                color: Theme.panel
                border.color: Theme.line
                radius: 4
                UniverseMeter {
                    anchors { fill: parent; margins: 4 }
                    occupancy: true
                    universe: sheet.meterUniverse
                    color: Theme.accent
                    conflictColor: Theme.err
                    highlightColor: Theme.warn
                }
            }
            Repeater {
                model: [[Theme.accent, qsTr("used")], [Theme.err, qsTr("conflict")], [Theme.warn, qsTr("selected")]]
                Label {
                    required property var modelData
                    textFormat: Text.StyledText
                    text: "<font color='" + modelData[0] + "'>■</font> " + modelData[1]
                    color: Theme.muted
                    font.pixelSize: Theme.small + 1
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.panel
            border.color: Theme.line
            radius: Theme.radius
            clip: true

            Row {
                id: head
                x: 8
                width: parent.width - 16
                height: 28
                spacing: 8
                Repeater {
                    model: sheet.columns
                    Label {
                        required property var modelData
                        required property int index
                        width: sheet.colWidth(index)
                        height: head.height
                        verticalAlignment: Text.AlignVCenter
                        text: modelData[1] + (sheet.sortKey === modelData[0] ? (sheet.ascending ? " ▲" : " ▼") : "")
                        color: sheet.sortKey === modelData[0] ? Theme.fg : Theme.muted
                        font.pixelSize: Theme.small + 1
                        elide: Text.ElideRight
                        TapHandler {
                            onTapped: {
                                if (sheet.sortKey === modelData[0]) sheet.ascending = !sheet.ascending
                                else { sheet.sortKey = modelData[0]; sheet.ascending = true }
                            }
                        }
                    }
                }
            }
            Rectangle { anchors.top: head.bottom; width: parent.width; height: 1; color: Theme.line }

            ListView {
                id: list
                anchors { fill: parent; topMargin: head.height + 1; margins: 1 }
                clip: true
                model: sheet.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: rowItem
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 26
                    color: modelData.selected ? Theme.accentSoft : hover.hovered ? Theme.control
                         : index % 2 ? Theme.bg : "transparent"
                    Rectangle { visible: rowItem.modelData.selected; width: 2; height: parent.height; color: Theme.accent }
                    HoverHandler { id: hover }
                    TapHandler {
                        onTapped: (point, button) => Session.selectUuid(rowItem.modelData.uuid,
                                                                         point.modifiers & (Qt.ControlModifier | Qt.MetaModifier))
                    }
                    Row {
                        x: 7
                        height: parent.height
                        spacing: 8
                        component Cell: Label {
                            property int col
                            width: sheet.colWidth(col)
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            color: Theme.fg
                            font.pixelSize: Theme.fontSize
                        }
                        Cell { col: 0; text: rowItem.modelData.fixtureId ?? "—"; font.family: Theme.mono }
                        Cell { col: 1; text: rowItem.modelData.name }
                        Cell { col: 2; text: rowItem.modelData.type; color: Theme.muted }
                        Cell { col: 3; text: rowItem.modelData.mode; color: Theme.muted }
                        TextField {
                            width: sheet.colWidth(4)
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 20
                            padding: 2
                            text: rowItem.modelData.patch
                            placeholderText: "—"
                            font.family: Theme.mono
                            font.pixelSize: Theme.small + 1
                            color: rowItem.modelData.conflict ? Theme.err : rowItem.modelData.overflow ? Theme.warn : Theme.fg
                            selectByMouse: true
                            background: Rectangle {
                                color: parent.activeFocus ? Theme.field : "transparent"
                                radius: 3
                                border.color: parent.activeFocus ? Theme.accent : "transparent"
                            }
                            onEditingFinished: if (text !== rowItem.modelData.patch && !Session.setPatch(rowItem.modelData.uuid, text))
                                                   text = rowItem.modelData.patch
                        }
                        Cell { col: 5; text: rowItem.modelData.footprint; font.family: Theme.mono; color: Theme.muted }
                        Cell { col: 6; text: sheet.fmt(rowItem.modelData.position); font.family: Theme.mono; color: Theme.muted }
                        Label {
                            width: 24
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.modelData.conflict ? "⚠" : rowItem.modelData.overflow ? "!" : ""
                            color: rowItem.modelData.conflict ? Theme.err : Theme.warn
                            font.pixelSize: Theme.fontSize
                            ToolTip.visible: tip.hovered && text !== ""
                            ToolTip.text: rowItem.modelData.conflict ? qsTr("Channels overlap another fixture")
                                                                      : qsTr("Footprint runs past channel 512")
                            HoverHandler { id: tip }
                        }
                    }
                }
            }
        }
    }
}
