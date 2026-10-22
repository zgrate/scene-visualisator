import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Project screen: new, open, save, save as, imports (GDTF, OBJ, Resolume) and exports (patch CSV, grandMA3 patch,
// Resolume Advanced Output, OBJ). Errors show inline under the buttons.
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
        onAccepted: screen.report(Session.importResolume(selectedFile), qsTr("Screens imported"))
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
            objSelection: ["obj", qsTr("Wavefront OBJ (*.obj)")]
        })
        fileMode: FileDialog.SaveFile
        defaultSuffix: kind ? formats[kind][0] : ""
        nameFilters: kind ? [formats[kind][1]] : []
        onAccepted: screen.report(Session.exportFile(kind, selectedFile), qsTr("Exported"))
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
        width: 460
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
            Section { text: qsTr("EXPORT") }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Button { text: qsTr("Patch CSV…"); onClicked: screen.exportAs("csv") }
                Button { text: qsTr("grandMA3 patch…"); onClicked: screen.exportAs("ma3") }
                Button { text: qsTr("Resolume output…"); onClicked: screen.exportAs("resolume") }
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
