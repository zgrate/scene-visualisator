import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Project screen: new, open, save, save as, imports (GDTF, OBJ, Resolume, MVR, patch CSV) and exports (patch CSV,
// grandMA3 patch, Resolume Advanced Output, OBJ, MVR). Errors show inline under the buttons.
Rectangle {
    id: screen
    color: Theme.bg
    property string message
    property bool failed: false

    function report(error, ok) {
        failed = error !== ""
        message = failed ? error : ok
    }
    function open() { openDialog.open() }
    function save() { if (Session.filePath) report(Session.saveProject(""), qsTr("Saved")); else saveDialog.open() }

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
        onAccepted: screen.report(Session.saveProject(selectedFile), qsTr("Saved"))
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

    Rectangle {
        anchors.centerIn: parent
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
                Button { text: qsTr("New"); onClicked: { Session.newProject(); screen.report("", qsTr("New project")) } }
                Button { text: qsTr("Open…"); onClicked: openDialog.open() }
                Button { text: qsTr("Save"); onClicked: screen.save() }
                Button { text: qsTr("Save as…"); onClicked: saveDialog.open() }
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
}
