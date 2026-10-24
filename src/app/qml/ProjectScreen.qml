import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Project screen: new, open, save, save as, imports (GDTF, OBJ, FBX, Resolume, MVR, patch CSV) and exports (patch
// CSV, grandMA3 patch, Resolume Advanced Output, OBJ, FBX, MVR). Errors show inline under the buttons.
Rectangle {
    id: screen
    color: Theme.bg
    property string message
    property bool failed: false
    // Runs an action that replaces the project; Main.qml sets it to its unsaved-changes check.
    property var guard: action => action()
    signal problem() // a failed action, so Main can bring this screen up to show the message
    signal wizard()  // Main opens the New project wizard behind its unsaved-changes guard

    function report(error, ok) {
        failed = error !== ""
        message = failed ? error : ok
        if (failed) problem()
    }
    function open() { guard(() => openDialog.open()) }
    function openUrl(url) { guard(() => report(Session.openProject(url), qsTr("Opened"))) }
    // then: run after a successful save (the unsaved-changes dialog continues with its action).
    function save(then) {
        if (!Session.filePath) return saveAs(then)
        const error = Session.saveProject("")
        report(error, qsTr("Saved"))
        if (!error && then) then()
    }
    function saveAs(then) { saveDialog.then = then || null; saveDialog.open() }

    FileDialog {
        id: openDialog
        nameFilters: [qsTr("Scene Visualiser projects (*.svproj)")]
        onAccepted: screen.report(Session.openProject(selectedFile), qsTr("Opened"))
    }
    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "svproj"
        nameFilters: [qsTr("Scene Visualiser projects (*.svproj)")]
        property var then: null
        onAccepted: {
            const error = Session.saveProject(selectedFile)
            screen.report(error, qsTr("Saved"))
            if (!error && then) then()
            then = null
        }
        onRejected: then = null
    }
    FileDialog {
        id: gdtfDialog
        nameFilters: [qsTr("GDTF fixture types (*.gdtf)")]
        onAccepted: screen.report(Session.importGdtf(selectedFile), qsTr("Fixture type imported"))
    }

    FileDialog {
        id: objDialog
        nameFilters: [qsTr("Wavefront OBJ (*.obj)")]
        onAccepted: screen.report(Session.importObj(selectedFile, unit.currentText, zUp.checked, split.checked),
                                  qsTr("Set piece imported"))
    }
    FileDialog {
        id: fbxDialog
        nameFilters: [qsTr("Autodesk FBX (*.fbx)")]
        onAccepted: screen.report(Session.importFbx(selectedFile), qsTr("FBX imported"))
    }
    FileDialog {
        id: resolumeDialog
        nameFilters: [qsTr("Resolume Advanced Output (*.xml)")]
        onAccepted: {
            const error = Session.importResolume(selectedFile, Number(pitch.text) || 3.9, place.checked)
            screen.report(error, Session.resolumeWarnings.length
                          ? qsTr("Screens imported · %1 warnings, see Mapping").arg(Session.resolumeWarnings.length)
                          : qsTr("Screens imported"))
        }
    }
    FileDialog {
        id: mvrDialog
        nameFilters: [qsTr("My Virtual Rig (*.mvr)")]
        onAccepted: {
            const error = Session.importMvr(selectedFile)
            screen.report(error, Session.resolumeWarnings.length
                          ? qsTr("MVR imported · %1 warnings: %2").arg(Session.resolumeWarnings.length)
                                                                     .arg(Session.resolumeWarnings.join("; "))
                          : qsTr("MVR imported"))
        }
    }
    FileDialog {
        id: csvDialog
        nameFilters: [qsTr("Patch lists (*.csv *.txt)")]
        onAccepted: {
            const p = Session.csvPreview(selectedFile)
            if (p.error) { screen.report(p.error, ""); return }
            csvMap.url = selectedFile
            csvMap.preview = p
            csvMap.mapping = Object.assign({}, p.mapping)
            csvMap.open()
        }
    }
    // FR-23: one ComboBox per field over the file's columns, guessed from the header.
    Dialog {
        id: csvMap
        property url url
        property var preview: ({ header: [], rows: [], fields: [], mapping: {}, count: 0 })
        property var mapping: ({})
        readonly property var labels: ({ fixtureId: qsTr("Fixture ID"), name: qsTr("Name"), type: qsTr("Type"),
                                         mode: qsTr("Mode"), universe: qsTr("Universe"), address: qsTr("Address"),
                                         patch: qsTr("Patch u.a"), x: "X", y: "Y", z: "Z", rotX: qsTr("Rot X"),
                                         rotY: qsTr("Rot Y"), rotZ: qsTr("Rot Z") })
        anchors.centerIn: parent
        modal: true
        title: qsTr("Import patch CSV · %1 rows").arg(preview.count)
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            spacing: 8
            GridLayout {
                columns: 4
                columnSpacing: 8
                rowSpacing: 4
                Repeater {
                    model: csvMap.preview.fields
                    RowLayout {
                        id: fieldRow
                        required property string modelData
                        spacing: 6
                        Label {
                            text: csvMap.labels[fieldRow.modelData] || fieldRow.modelData
                            color: Theme.muted
                            font.pixelSize: Theme.small + 1
                            Layout.preferredWidth: 64
                        }
                        ComboBox {
                            implicitWidth: 130
                            font.pixelSize: Theme.small + 1
                            model: ["—"].concat(csvMap.preview.header)
                            currentIndex: (csvMap.mapping[fieldRow.modelData] ?? -1) + 1
                            onActivated: i => {
                                const m = Object.assign({}, csvMap.mapping)
                                m[fieldRow.modelData] = i - 1
                                csvMap.mapping = m
                            }
                        }
                    }
                }
            }
            RowLayout {
                spacing: 6
                Label { text: qsTr("Positions in"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                ComboBox { id: csvUnit; implicitWidth: 64; model: ["m", "cm", "mm", "in", "ft"] }
                Check { id: csvZUp; text: qsTr("Z up (Vectorworks)") }
            }
            Label {
                text: [csvMap.preview.header.join(" | ")].concat(csvMap.preview.rows.map(r => r.join(" | "))).join("\n")
                color: Theme.fg
                font.family: Theme.mono
                font.pixelSize: Theme.small
                elide: Text.ElideRight
                Layout.maximumWidth: 640
            }
        }
        onAccepted: screen.report(Session.importCsv(url, mapping, csvUnit.currentText, csvZUp.checked),
                                  qsTr("Patch imported"))
    }
    // One save dialog for every export; kind picks the format, suffix and filter.
    FileDialog {
        id: exportDialog
        property string kind
        readonly property var formats: ({
            csv: ["csv", qsTr("Patch CSV (*.csv)")],
            ma3: ["xml", qsTr("grandMA3 patch (*.xml)")],
            resolume: ["xml", qsTr("Resolume Advanced Output (*.xml)")],
            obj: ["obj", qsTr("Wavefront OBJ (*.obj)")],
            objSelection: ["obj", qsTr("Wavefront OBJ (*.obj)")],
            fbx: ["fbx", qsTr("Autodesk FBX (*.fbx)")],
            fbxSelection: ["fbx", qsTr("Autodesk FBX (*.fbx)")],
            mvr: ["mvr", qsTr("My Virtual Rig (*.mvr)")]
        })
        fileMode: FileDialog.SaveFile
        defaultSuffix: kind ? formats[kind][0] : ""
        nameFilters: kind ? [formats[kind][1]] : []
        onAccepted: screen.report(Session.exportFile(kind, selectedFile), qsTr("Exported"))
    }
    Dialog {
        id: overwrite
        property var preset: ({})
        anchors.centerIn: parent
        modal: true
        title: qsTr("Replace the Resolume preset?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: overwrite.preset.path || ""; color: Theme.fg; font.family: Theme.mono; font.pixelSize: Theme.small + 1 }
        onAccepted: screen.report(Session.exportFile("resolume", preset.url), qsTr("Preset written: %1").arg(preset.path))
    }
    function exportAs(kind) { exportDialog.kind = kind; exportDialog.open() }

    component Section: Label {
        color: Theme.muted
        font.pixelSize: Theme.small
        font.letterSpacing: 1
        topPadding: 6
    }

    // Scrolls when the window is shorter than the cards; centred otherwise.
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, cards.height + 32)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        Row {
            id: cards
            anchors.centerIn: parent
            spacing: 16
            Rectangle {
                width: 540
                height: col.implicitHeight + 32
                color: Theme.panel
                border.color: Theme.line
                radius: Theme.radius
                ColumnLayout {
                    id: col
                    anchors { fill: parent; margins: 16 }
                    spacing: 10
                    Label { text: qsTr("PROJECT"); color: Theme.muted; font.pixelSize: Theme.small; font.letterSpacing: 1 }
                    Label {
                        text: Session.projectName + (Session.dirty ? " •" : "")
                        color: Theme.fg
                        font.pixelSize: 16
                        font.bold: true
                    }
                    Label {
                        text: Session.filePath || qsTr("Not saved yet")
                        color: Theme.muted
                        font.family: Theme.mono
                        font.pixelSize: Theme.small + 1
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        spacing: 6
                        Button {
                            text: qsTr("New")
                            onClicked: screen.guard(() => { Session.newProject(); screen.report("", qsTr("New project")) })
                        }
                        Button { text: qsTr("New from wizard…"); onClicked: screen.wizard() }
                        Button { text: qsTr("Open…"); onClicked: screen.open() }
                        Button { text: qsTr("Save"); onClicked: screen.save() }
                        Button { text: qsTr("Save as…"); onClicked: screen.saveAs() }
                    }
                    RowLayout {
                        visible: Session.linkedMedia.length > 0
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Linked, not in the project: %1").arg(Session.linkedMedia.join(", "))
                            color: Theme.warn
                            elide: Text.ElideMiddle
                            font.pixelSize: Theme.small + 1
                        }
                        Button {
                            text: qsTr("Pack into project")
                            onClicked: screen.report(Session.packMedia(), qsTr("Media packed; save to keep it"))
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Copies the file into the .svproj so the project opens on another computer")
                        }
                    }
                    Section { text: qsTr("RECENT"); visible: Session.recentFiles.length > 0 }
                    Repeater {
                        model: Session.recentFiles
                        delegate: ItemDelegate {
                            required property var modelData
                            Layout.fillWidth: true
                            padding: 4
                            contentItem: RowLayout {
                                spacing: 8
                                Label { text: modelData.name; color: Theme.fg; font.pixelSize: Theme.small + 2 }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.path
                                    color: Theme.muted
                                    elide: Text.ElideMiddle
                                    font.family: Theme.mono
                                    font.pixelSize: Theme.small
                                }
                            }
                            onClicked: screen.openUrl(modelData.url)
                        }
                    }
                    Section { text: qsTr("IMPORT") }
                    RowLayout {
                        spacing: 6
                        Button { text: qsTr("GDTF…"); onClicked: gdtfDialog.open() }
                        Button { text: qsTr("Resolume screens…"); onClicked: resolumeDialog.open() }
                        Label { text: qsTr("pitch mm"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                        Input {
                            id: pitch
                            implicitWidth: 48
                            text: "3.9"
                            font.family: Theme.mono
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Pixel pitch for new walls and strips; also scales their placement")
                        }
                        Check { id: place; text: qsTr("Place from composition"); checked: true }
                    }
                    RowLayout {
                        spacing: 6
                        Button { text: qsTr("OBJ set piece…"); onClicked: objDialog.open() }
                        ComboBox {
                            id: unit
                            implicitWidth: 64
                            model: ["m", "cm", "mm", "in", "ft"]
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Unit the OBJ file is in")
                        }
                        Check { id: zUp; text: qsTr("Z up") }
                        Check { id: split; text: qsTr("Split groups") }
                    }
                    RowLayout {
                        spacing: 6
                        Button { text: qsTr("MVR scene…"); onClicked: mvrDialog.open() }
                        Button {
                            text: qsTr("FBX scene…")
                            onClicked: fbxDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Set pieces per mesh, hierarchy kept; units and axes converted")
                        }
                        Button {
                            text: qsTr("Patch CSV…")
                            onClicked: csvDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Our patch CSV, Capture or Vectorworks lists; matching fixture IDs are updated")
                        }
                    }
                    Section { text: qsTr("EXPORT") }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Button { text: qsTr("Patch CSV…"); onClicked: screen.exportAs("csv") }
                        Button { text: qsTr("grandMA3 patch…"); onClicked: screen.exportAs("ma3") }
                        Button { text: qsTr("Resolume output…"); onClicked: screen.exportAs("resolume") }
                        Button {
                            text: qsTr("Send to Resolume")
                            onClicked: {
                                const p = Session.resolumePreset()
                                if (p.exists) { overwrite.preset = p; overwrite.open() }
                                else screen.report(Session.exportFile("resolume", p.url), qsTr("Preset written: %1").arg(p.path))
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Writes an Advanced Output preset; pick it once in Arena's Advanced Output")
                        }
                        Button { text: qsTr("MVR scene…"); onClicked: screen.exportAs("mvr") }
                        Button { text: qsTr("OBJ scene…"); onClicked: screen.exportAs("obj") }
                        Button {
                            text: qsTr("OBJ selection…")
                            enabled: Session.selectionCount > 0
                            onClicked: screen.exportAs("objSelection")
                        }
                        Button { text: qsTr("FBX scene…"); onClicked: screen.exportAs("fbx") }
                        Button {
                            text: qsTr("FBX selection…")
                            enabled: Session.selectionCount > 0
                            onClicked: screen.exportAs("fbxSelection")
                        }
                    }
                    Section { text: qsTr("VIEWPORT") }
                    RowLayout {
                        spacing: 6
                        Label { text: qsTr("Controls"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                        ComboBox {
                            implicitWidth: 170
                            model: [qsTr("Blender"), qsTr("Maya / industry")]
                            currentIndex: Session.controls === "maya" ? 1 : 0
                            onActivated: i => Session.setPreference("controls", i ? "maya" : "blender")
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: Session.controls === "maya"
                                  ? qsTr("Alt + left/middle/right drag orbits, pans, zooms. W E R Q pick gizmo handles, F frames.")
                                  : qsTr("Middle drag orbits (Alt + left on a trackpad), Shift pans, Ctrl zooms. G R S move, "
                                         + "rotate, scale (X Y Z lock, type a number, Enter). X deletes, Shift+D duplicates, "
                                         + "A selects all, numpad . frames.")
                            color: Theme.muted
                            font.pixelSize: Theme.small + 1
                        }
                    }
                    Check {
                        text: qsTr("Emulate numpad (top-row digits, - and =) · ? or F1 lists every key")
                        checked: String(Session.preference("emulateNumpad", false)) === "true"
                        onToggled: Session.setPreference("emulateNumpad", checked)
                    }
                    // Proxy: what the view shows (live input, the take where its scopes say, held lights) goes back out.
                    Section { text: qsTr("SIGNAL OUTPUT") }
                    RowLayout {
                        spacing: 6
                        Check {
                            text: qsTr("Send DMX")
                            checked: Session.outputStatus.enabled === true
                            onToggled: Session.setPreference("outputEnabled", checked)
                        }
                        ComboBox {
                            implicitWidth: 90
                            model: ["Art-Net", "sACN"]
                            currentIndex: Session.outputStatus.protocol === "sacn" ? 1 : 0
                            onActivated: i => Session.setPreference("outputProtocol", i ? "sacn" : "artnet")
                        }
                        Input {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 80
                            placeholderText: qsTr("broadcast / multicast")
                            text: Session.outputStatus.target || ""
                            font.family: Theme.mono
                            onEditingFinished: Session.setPreference("outputTarget", text.trim())
                        }
                        Label { text: qsTr("Universe +"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                        Input {
                            implicitWidth: 50
                            text: String(Session.outputStatus.offset || 0)
                            font.family: Theme.mono
                            validator: IntValidator { bottom: 0; top: 32767 }
                            onEditingFinished: Session.setPreference("outputUniverseOffset", Number(text))
                        }
                        Label {
                            visible: Session.outputStatus.protocol === "sacn"
                            text: qsTr("Priority")
                            color: Theme.muted
                            font.pixelSize: Theme.small + 1
                        }
                        Input {
                            visible: Session.outputStatus.protocol === "sacn"
                            implicitWidth: 40
                            text: String(Session.preference("outputPriority", 100))
                            font.family: Theme.mono
                            validator: IntValidator { bottom: 0; top: 200 }
                            onEditingFinished: Session.setPreference("outputPriority", Number(text))
                        }
                    }
                    RowLayout {
                        spacing: 6
                        Check {
                            text: qsTr("Send Resolume outputs as NDI (\"SV <output>\")")
                            checked: String(Session.preference("outputNdi", false)) === "true"
                            onToggled: Session.setPreference("outputNdi", checked)
                        }
                        Label {
                            readonly property var out: Session.outputStatus
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: out.error ? out.error
                                  : out.enabled ? qsTr("%1 universes · %2 packets/s").arg(out.universes).arg(Math.round(out.rate))
                                                  + (out.ndi.length ? " · NDI " + out.ndi.map(n => n.name).join(", ") : "")
                                  : out.ndi && out.ndi.length ? "NDI " + out.ndi.map(n => n.name).join(", ") : ""
                            color: out.error ? Theme.err : Theme.muted
                            font.family: Theme.mono
                            font.pixelSize: Theme.small
                        }
                    }
                    Section { text: qsTr("VENUE") }
                    RowLayout {
                        spacing: 6
                        Repeater {
                            model: [["width", qsTr("Width"), 20], ["depth", qsTr("Depth"), 15], ["height", qsTr("Height"), 8]]
                            RowLayout {
                                required property var modelData
                                spacing: 4
                                Label { text: modelData[1]; color: Theme.muted; font.pixelSize: Theme.small + 1 }
                                Input {
                                    implicitWidth: 52
                                    font.family: Theme.mono
                                    text: Number(Session.venue[modelData[0]] ?? modelData[2]).toString()
                                    onEditingFinished: {
                                        const v = parseFloat(text)
                                        if (v > 0 && v !== Session.venue[modelData[0]])
                                            Session.edit("venue", "/" + modelData[0], v)
                                    }
                                }
                            }
                        }
                        Label { text: qsTr("m"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                        HudButton {
                            text: qsTr("Fit to geometry")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Size the floor and height to everything in the scene")
                            onClicked: Session.fitVenue()
                        }
                    }
                    RowLayout {
                        spacing: 6
                        // Fog machines and hazers in the rig raise it while they run.
                        Label { text: qsTr("Haze (minimum)"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
                        Slider {
                            id: haze
                            implicitWidth: 140
                            from: 0; to: 1
                            value: Session.venue.haze ?? 0.35
                            onPressedChanged: if (!pressed) Session.edit("venue", "/haze", Math.round(value * 100) / 100)
                        }
                        Label { text: haze.value.toFixed(2); color: Theme.fg; font.family: Theme.mono; font.pixelSize: Theme.small + 1 }
                    }
                    Section { text: qsTr("ABOUT") }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Scene Visualiser %1 · LGPL-3.0-or-later. Qt 6 (LGPL v3, shared libraries). NDI® runtime "
                                   + "not shipped, SDK headers MIT; Spout reader own code; Syphon BSD; Sentry MIT.")
                              .arg(Session.appInfo.version)
                        color: Theme.muted
                        font.pixelSize: Theme.small + 1
                    }
                    RowLayout {
                        spacing: 6
                        visible: Session.appInfo.updates
                        Button { text: qsTr("Check for updates"); onClicked: Session.checkForUpdates() }
                        Check {
                            text: qsTr("Automatically")
                            checked: Session.preference("autoUpdate", false)
                            onToggled: Session.setPreference("autoUpdate", checked)
                        }
                        ComboBox {
                            implicitWidth: 90
                            model: [qsTr("Stable"), qsTr("Test")]
                            currentIndex: Session.preference("updateChannel", "stable") === "test" ? 1 : 0
                            onActivated: i => Session.setPreference("updateChannel", i ? "test" : "stable")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Update channel; applies at the next launch")
                        }
                    }
                    Check {
                        visible: Session.appInfo.crashUploads
                        text: qsTr("Send crash reports")
                        checked: Session.preference("crashReports", false)
                        onToggled: Session.setPreference("crashReports", checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Crash reports: %1").arg(Session.appInfo.crashFolder)
                        color: Theme.muted
                        font.family: Theme.mono
                        font.pixelSize: Theme.small + 1
                        elide: Text.ElideMiddle
                    }
                    Label {
                        visible: screen.message !== ""
                        text: screen.message
                        color: screen.failed ? Theme.err : Theme.ok
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }
            AiPanel {}
        }
    }
}
