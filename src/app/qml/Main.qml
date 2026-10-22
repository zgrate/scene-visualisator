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

    // Every action that replaces the project (new, open, recent, drop, close) runs through here, so unsaved edits are
    // never dropped without asking.
    function guarded(action) {
        if (!Session.dirty) return action()
        unsaved.action = action
        unsaved.open()
    }
    // Files dropped on the window or handed over by the OS: a project opens, anything else is imported.
    function openFiles(urls) {
        for (const url of urls) {
            const name = url.toString().toLowerCase()
            const ext = name.slice(name.lastIndexOf(".") + 1)
            if (ext === "svproj") { project.openUrl(url); return }
            const error = ext === "gdtf" ? Session.importGdtf(url)
                        : ext === "mvr" ? Session.importMvr(url)
                        : ext === "obj" ? Session.importObj(url, "m", false, false)
                        : ext === "fbx" ? Session.importFbx(url)
                        : ext === "xml" ? Session.importResolume(url)
                        : qsTr("Cannot open .%1 files by dropping; use the Project screen").arg(ext)
            project.report(error, qsTr("Imported %1").arg(decodeURIComponent(name.slice(name.lastIndexOf("/") + 1))))
        }
    }
    property bool closeConfirmed: false
    onClosing: close => {
        if (Session.dirty && !closeConfirmed) {
            close.accepted = false
            guarded(() => { closeConfirmed = true; win.close() })
        }
    }

    Shortcut { sequences: [StandardKey.Undo]; onActivated: Session.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; onActivated: Session.redo() }
    Shortcut { sequences: [StandardKey.Delete, "Backspace"]; onActivated: Session.deleteSelection() }
    Shortcut { sequence: "Ctrl+D"; onActivated: Session.duplicateSelection() }
    Shortcut { sequence: "Escape"; onActivated: Session.selectUuid("", false) }
    // Arrows nudge 0.25 m on the floor plane (Up = upstage, -Z); Page Up/Down raise and lower. Off while typing.
    readonly property bool typing: activeFocusItem instanceof TextInput
    Shortcut { sequence: "Left"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(-0.25, 0, 0) }
    Shortcut { sequence: "Right"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(0.25, 0, 0) }
    Shortcut { sequence: "Up"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(0, 0, -0.25) }
    Shortcut { sequence: "Down"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(0, 0, 0.25) }
    Shortcut { sequence: "PgUp"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(0, 0.25, 0) }
    Shortcut { sequence: "PgDown"; enabled: !win.typing && win.screen !== "mapping"; onActivated: Session.nudgeSelection(0, -0.25, 0) }
    Shortcut { sequences: [StandardKey.Open]; onActivated: project.open() }
    Shortcut { sequences: [StandardKey.Save]; onActivated: project.save() }
    Shortcut { sequences: [StandardKey.SaveAs, "Ctrl+Shift+S"]; onActivated: project.saveAs() }

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
                model: [["design", qsTr("Design")], ["patch", qsTr("Patch")], ["dmx", qsTr("DMX")], ["plan", qsTr("Plan")], ["mapping", qsTr("Mapping")]]
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
            Label { // AI access: green while a client has called in the last 10 s
                visible: Session.aiStatus.listening
                textFormat: Text.StyledText
                text: "AI <font color='" + (Session.aiStatus.active ? Theme.ok : Theme.line) + "'>●</font>"
                color: Theme.muted
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
                ToolTip.visible: aiHover.hovered
                ToolTip.text: qsTr("AI access: %1 client(s), last call %2").arg(Session.aiStatus.clients)
                              .arg(Session.aiStatus.lastTool || "–")
                HoverHandler { id: aiHover }
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
            Rectangle { width: 1; Layout.fillHeight: true; color: Theme.line }
            Button {
                id: mediaButton
                readonly property var st: Session.mediaStatus
                readonly property var names: ({ testPattern: qsTr("Test pattern"), grid: qsTr("Mapping grid"),
                                                still: qsTr("Still"), video: qsTr("Video"), screen: qsTr("Screen"),
                                                window: qsTr("Window") })
                implicitHeight: 24
                flat: true
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
                text: qsTr("MEDIA") + " · " + (names[st.kind] || st.kind) + (!st.live ? ""
                      : st.signal ? " · %1×%2 · %3 fps".arg(st.width).arg(st.height).arg(st.fps.toFixed(1))
                      : " · " + qsTr("No signal"))
                palette.buttonText: st.live && !st.signal ? Theme.warn : Theme.muted
                onClicked: media.open()
                MediaCard { id: media; y: -height - 10; x: parent.width - width }
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
                        ["plan", qsTr("Plan and elevation")], ["mapping", qsTr("Mapping")], ["project", qsTr("Project")]]
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
            objectName: "viewport" // found by the AI screenshot and camera tools
            anchors.fill: parent
        }

        // Debug mode HUD: renderer, DMX input, media and AI access at a glance.
        Rectangle {
            visible: Session.debugMode && win.screen === "design"
            anchors { left: parent.left; bottom: viewLabel.top; margins: Theme.gap }
            width: hud.implicitWidth + 16
            height: hud.implicitHeight + 12
            color: Theme.panel
            opacity: 0.92
            border.color: Theme.line
            radius: Theme.radius
            Label {
                id: hud
                anchors.centerIn: parent
                readonly property var r: Session.renderStats
                readonly property var d: Session.dmxStatus
                readonly property var m: Session.mediaStatus
                readonly property var a: Session.aiStatus
                text: [
                    "DEBUG",
                    "%1 fps · cpu %2 ms".arg((r.fps || 0).toFixed(1)).arg((r.cpuMs || 0).toFixed(2)),
                    "boxes %1 · beams %2 · walls %3 · meshes %4".arg(r.boxes || 0).arg(r.beams || 0)
                        .arg(r.walls || 0).arg(r.meshes || 0),
                    (r.backend || "?") + " · " + (r.device || "").slice(0, 40)
                ].concat(Object.keys(d.perUniverse || {}).map(u => {
                    const p = d.perUniverse[u]
                    return "U%1 %2 Hz · lost %3 · dropped %4 · %5".arg(u).arg(p.rate).arg(p.lost).arg(p.dropped)
                        .arg(p.sources.join(", ") || "no source")
                })).concat([
                    m.kind ? "media %1 %2×%3 · %4 ms".arg(m.kind).arg(m.width).arg(m.height)
                                 .arg((m.latency || 0).toFixed(1)) : "media –",
                    a.listening ? "AI %1 client(s) · %2 call(s) · %3".arg(a.clients).arg(a.calls).arg(a.lastTool || "–")
                                : "AI off"
                ]).join("\n")
                color: Theme.fg
                font.family: Theme.mono
                font.pixelSize: Theme.small
            }
        }

        // HUD: view name and camera presets / bookmarks.
        Label {
            id: viewLabel
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
            guard: win.guarded
            onProblem: win.screen = "project"
        }

        PatchScreen {
            visible: win.screen === "patch"
            anchors.fill: parent
        }

        DmxScreen {
            visible: win.screen === "dmx"
            anchors.fill: parent
        }

        MappingScreen {
            visible: win.screen === "mapping"
            anchors.fill: parent
        }

        PlanScreen {
            visible: win.screen === "plan"
            anchors.fill: parent
        }
    }

    DropArea {
        anchors.fill: parent
        onDropped: drop => { if (drop.hasUrls) { drop.accept(Qt.CopyAction); win.openFiles(drop.urls) } }
    }

    Dialog {
        id: unsaved
        property var action: null
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Save changes to “%1”?").arg(Session.projectName)
        Label {
            text: qsTr("Your changes are lost if you don't save them.")
            color: Theme.fg
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Save"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { text: qsTr("Don't save"); DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole }
            Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onDiscarded: {
                unsaved.close()
                Session.discardChanges()
                unsaved.action()
            }
        }
        onAccepted: project.save(action)
    }

    // Opened files whose fixture types, geometry or media are not where the project says.
    Dialog {
        id: missing
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Some files were not found")
        standardButtons: Dialog.Ok
        Label {
            width: 460
            wrapMode: Text.Wrap
            text: qsTr("These are drawn as placeholders until they are imported again:\n%1")
                  .arg(Session.missingFiles.join("\n"))
            color: Theme.fg
        }
        function check() { if (Session.missingFiles.length && !opened) open() }
        Component.onCompleted: check() // a project given on the command line opened before QML existed
    }

    // AT-6: an autosave newer than the saved file (or an untitled one) is offered once on launch or open.
    Dialog {
        id: recovery
        anchors.centerIn: parent
        modal: true
        title: qsTr("Recover unsaved work?")
        closePolicy: Popup.NoAutoClose
        Label {
            text: qsTr("An autosave newer than the project was found:\n%1").arg(Session.recoveryFile)
            color: Theme.fg
            wrapMode: Text.Wrap
            width: 420
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Recover"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { text: qsTr("Discard"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        onAccepted: {
            const error = Session.recover()
            if (error) { project.report(error, ""); win.screen = "project" }
        }
        onRejected: Session.discardRecovery()
        function check() { if (Session.recoveryFile !== "" && !opened) open() }
        Component.onCompleted: check()
    }
    Connections {
        target: Session
        function onChanged() { recovery.check() }
        function onProjectOpened() { missing.check() }
        function onOpenRequested(url) { win.openFiles([url]) }
    }
}
