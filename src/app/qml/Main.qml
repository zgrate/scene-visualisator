import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// Studio posture (design 2), dark: the 3D view is the page; a slim icon rail, floating Objects and Inspector
// cards, and a DMX meter dock. Patch, DMX and Project screens replace the viewport area; the viewport stays alive.
ApplicationWindow {
    id: win
    width: 1440
    height: 900
    visible: true
    title: Session.projectName + " — Scene Visualiser"
    color: Theme.bg
    // Stock Fusion controls (combo boxes, menus, buttons, scroll bars) take the Studio dark tokens from here.
    palette {
        window: Theme.panel
        windowText: Theme.fg
        base: Theme.field
        alternateBase: Theme.bg
        text: Theme.fg
        button: Theme.control
        buttonText: Theme.fg
        brightText: Theme.fg
        placeholderText: Theme.muted
        highlight: Theme.accent
        highlightedText: Theme.accentOn
        toolTipBase: Theme.panel
        toolTipText: Theme.fg
        light: Theme.line
        midlight: Theme.line
        mid: Theme.line
        dark: Theme.bg
        shadow: "black"
        disabled.text: Theme.muted
        disabled.buttonText: Theme.muted
        disabled.windowText: Theme.muted
    }

    property string screen: "design"
    property int dockUniverse: Session.universes.length ? Session.universes[0] : 1

    function rate(v) { return v < 0 ? "—" : Math.round(v) + " Hz" }

    Shortcut { sequences: [StandardKey.Undo]; onActivated: Session.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; onActivated: Session.redo() }
    Shortcut { sequences: [StandardKey.Delete, "Backspace"]; onActivated: Session.deleteSelection() }
    Shortcut { sequence: "Ctrl+D"; onActivated: Session.duplicateSelection() }
    Shortcut { sequence: "Escape"; onActivated: Session.selectUuid("", false) }
    // Arrows nudge 0.25 m on the floor plane (Up = upstage, -Z); Page Up/Down raise and lower. Off while typing.
    readonly property bool typing: activeFocusItem instanceof TextInput
    Shortcut { sequence: "Left"; enabled: !win.typing; onActivated: Session.nudgeSelection(-0.25, 0, 0) }
    Shortcut { sequence: "Right"; enabled: !win.typing; onActivated: Session.nudgeSelection(0.25, 0, 0) }
    Shortcut { sequence: "Up"; enabled: !win.typing; onActivated: Session.nudgeSelection(0, 0, -0.25) }
    Shortcut { sequence: "Down"; enabled: !win.typing; onActivated: Session.nudgeSelection(0, 0, 0.25) }
    Shortcut { sequence: "PgUp"; enabled: !win.typing; onActivated: Session.nudgeSelection(0, 0.25, 0) }
    Shortcut { sequence: "PgDown"; enabled: !win.typing; onActivated: Session.nudgeSelection(0, -0.25, 0) }
    Shortcut { sequences: [StandardKey.Open]; onActivated: project.open() }
    Shortcut { sequences: [StandardKey.Save]; onActivated: project.save() }

    header: Rectangle {
        height: Theme.titleBar
        color: Theme.panel
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        RowLayout {
            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
            spacing: 14
            Label {
                text: Session.projectName + (Session.dirty ? " •" : "")
                color: Theme.fg
                font.bold: true
                font.pixelSize: Theme.fontSize
            }
            Repeater {
                model: [["design", qsTr("Design")], ["patch", qsTr("Patch")], ["dmx", qsTr("DMX")]]
                Label {
                    required property var modelData
                    text: modelData[1]
                    color: win.screen === modelData[0] ? Theme.accent : Theme.muted
                    font.bold: win.screen === modelData[0]
                    font.pixelSize: Theme.fontSize
                    TapHandler { onTapped: win.screen = modelData[0] }
                }
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [["Art-Net", Session.dmxStatus.artnet], ["sACN", Session.dmxStatus.sacn]]
                Label {
                    required property var modelData
                    textFormat: Text.StyledText
                    text: modelData[0] + " <font color='" + (modelData[1] < 0 ? Theme.line : Theme.ok) + "'>●</font> "
                          + win.rate(modelData[1])
                    color: Theme.muted
                    font.family: Theme.mono
                    font.pixelSize: Theme.small + 1
                }
            }
        }
    }

    footer: Rectangle {
        height: Theme.dock
        color: Theme.panel
        Rectangle { width: parent.width; height: 1; color: Theme.line }
        RowLayout {
            anchors { fill: parent; leftMargin: 12; rightMargin: 12; topMargin: 7; bottomMargin: 7 }
            spacing: 10
            Label {
                text: qsTr("DMX · UNIVERSE")
                color: Theme.muted
                font.family: Theme.mono
                font.pixelSize: Theme.small
                font.letterSpacing: 1
            }
            ComboBox {
                implicitWidth: 64
                implicitHeight: 24
                font.pixelSize: Theme.small + 1
                model: Session.universes
                currentIndex: Session.universes.indexOf(win.dockUniverse)
                onActivated: i => win.dockUniverse = Session.universes[i]
            }
            UniverseMeter {
                Layout.fillWidth: true
                Layout.fillHeight: true
                universe: win.dockUniverse
                color: Theme.accent
            }
            Label {
                readonly property var u: Session.dmxStatus.perUniverse[win.dockUniverse] || { rate: 0, sources: [] }
                text: (u.sources.length ? u.sources.join(", ") : qsTr("no source")) + " · " + win.rate(u.rate)
                color: Theme.muted
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
                Layout.preferredWidth: 240
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
            }
        }
    }

    Rectangle {
        id: rail
        width: Theme.rail
        height: parent.height
        color: Theme.panel
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
        Column {
            y: 8
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 6
            Repeater {
                model: [["design", qsTr("Design")], ["patch", qsTr("Patch sheet")], ["dmx", qsTr("DMX monitor")],
                        ["project", qsTr("Project")]]
                RailButton {
                    required property var modelData
                    text: modelData[1]
                    icon.source: "icons/" + modelData[0] + ".svg"
                    active: win.screen === modelData[0]
                    onClicked: win.screen = modelData[0]
                }
            }
        }
    }

    Item {
        id: stage
        anchors { left: rail.right; right: parent.right; top: parent.top; bottom: parent.bottom }

        ViewportItem {
            id: viewport
            anchors.fill: parent
        }

        // HUD: view name and camera presets / bookmarks.
        Label {
            visible: win.screen === "design"
            anchors { left: parent.left; bottom: parent.bottom; margins: Theme.gap }
            text: viewport.view + " · FOH"
            color: Theme.muted
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
        }
        Row {
            visible: win.screen === "design"
            anchors { right: parent.right; bottom: parent.bottom; margins: Theme.gap }
            spacing: 4
            Repeater {
                model: ["Perspective", "Top", "Front", "Side"]
                HudButton {
                    required property string modelData
                    text: modelData
                    active: viewport.view === modelData
                    onClicked: viewport.setPreset(modelData)
                }
            }
            Item { width: 8; height: 1 }
            Repeater {
                model: Session.layout.cameraBookmarks || []
                HudButton {
                    required property var modelData
                    required property int index
                    text: String(index + 1)
                    onClicked: viewport.setCameraState(modelData)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Bookmark %1 (right-click removes)").arg(index + 1)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            const b = (Session.layout.cameraBookmarks || []).slice()
                            b.splice(index, 1)
                            Session.setLayout("cameraBookmarks", b)
                        }
                    }
                }
            }
            HudButton {
                text: "+"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Bookmark this camera")
                onClicked: Session.setLayout("cameraBookmarks",
                                             (Session.layout.cameraBookmarks || []).concat([viewport.cameraState()]))
            }
        }

        Item {
            id: cards
            anchors { fill: parent; margins: Theme.gap }
            visible: win.screen === "design" && stage.width > 900
            Card {
                key: "objects"
                title: qsTr("Objects")
                Objects {}
            }
            Card {
                key: "inspector"
                title: Session.current.name || qsTr("Inspector")
                width: 280
                defaultX: cards.width - width
                Inspector {}
            }
        }

        ProjectScreen {
            id: project
            visible: win.screen === "project"
            anchors.fill: parent
        }

        Rectangle {
            visible: win.screen === "patch" || win.screen === "dmx"
            anchors.fill: parent
            color: Theme.bg
            Label {
                anchors.centerIn: parent
                text: qsTr("This screen arrives in M6")
                color: Theme.muted
            }
        }
    }
}
