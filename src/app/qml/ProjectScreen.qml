import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Project screen: new, open, save, save as, import GDTF. Errors show inline under the buttons.
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

    Rectangle {
        anchors.centerIn: parent
        width: 420
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
            Button { text: qsTr("Import GDTF…"); onClicked: gdtfDialog.open() }
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
