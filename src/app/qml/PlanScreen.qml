import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SceneVisualiser

// FR-19 plan and elevation sheets: plan, front or side view of the rig with fixture symbols, automatic dimensions and
// a title block. Sheet size, scale, hidden layers, title, options, dimensions and notes are stored per view in
// project["drawings"]. On the sheet, clicks select and drag the rig, Shift-drag adds a dimension and the note tool
// places a note (DrawingItem). Exports the same drawing to PDF or SVG.
Rectangle {
    id: plan
    color: Theme.bg
    property string kind: "plan"
    // Reads Session.drawings in the expression so the binding re-runs on every edit (a bare statement is dropped)
    readonly property var sheet: Session.drawings && Session.drawing(kind)
    property string message
    property bool failed: false

    readonly property var scales: [0, 20, 25, 50, 100, 200]
    readonly property var defaultTitles: ({ plan: qsTr("Plan"), front: qsTr("Front elevation"),
                                            side: qsTr("Side elevation") })

    // Note text editor: index -1 adds `note` with the typed text; empty text deletes a note, or drops a new one.
    Popup {
        id: noteEditor
        property int index: -1
        property var note: ({})
        property bool accepted: false
        padding: 8
        focus: true
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
        onOpened: { accepted = false; noteText.text = note.text || ""; noteText.forceActiveFocus() }
        onClosed: {
            if (!accepted)
                return
            const notes = (plan.sheet.notes || []).slice()
            const text = noteText.text.trim()
            if (index < 0) {
                if (text === "")
                    return
                const n = Object.assign({}, note)
                n.text = text
                notes.push(n)
            } else if (text === "") {
                notes.splice(index, 1)
            } else {
                const n = Object.assign({}, notes[index])
                n.text = text
                notes[index] = n
            }
            Session.setDrawing(plan.kind, "notes", notes)
        }
        Input {
            id: noteText
            implicitWidth: 240
            placeholderText: qsTr("Note text, Enter to keep")
            onAccepted: { noteEditor.accepted = true; noteEditor.close() }
        }
    }

    FileDialog {
        id: exportDialog
        property string format: "pdf"
        fileMode: FileDialog.SaveFile
        defaultSuffix: format
        nameFilters: [format === "pdf" ? qsTr("PDF (*.pdf)") : qsTr("SVG (*.svg)")]
        onAccepted: {
            const error = Session.exportDrawing(plan.kind, selectedFile)
            plan.failed = error !== ""
            plan.message = error || qsTr("Exported")
        }
    }

    ColumnLayout {
        anchors { fill: parent; margins: 16 }
        spacing: 10

        RowLayout {
            spacing: 6
            Repeater {
                model: [["plan", qsTr("Plan")], ["front", qsTr("Front")], ["side", qsTr("Side")]]
                Button {
                    required property var modelData
                    text: modelData[1]
                    highlighted: plan.kind === modelData[0]
                    onClicked: plan.kind = modelData[0]
                }
            }
            Item { implicitWidth: 10 }
            Label { text: qsTr("Sheet"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            ComboBox {
                implicitWidth: 70
                model: ["A3", "A4"]
                currentIndex: Math.max(0, model.indexOf(plan.sheet.sheet))
                onActivated: Session.setDrawing(plan.kind, "sheet", currentText)
            }
            Label { text: qsTr("Scale"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            ComboBox {
                implicitWidth: 80
                model: plan.scales.map(n => n ? "1:" + n : qsTr("Fit"))
                currentIndex: Math.max(0, plan.scales.indexOf(plan.sheet.scale))
                onActivated: index => Session.setDrawing(plan.kind, "scale", plan.scales[index])
            }
            Repeater {
                model: Session.layers
                Check {
                    required property var modelData
                    text: modelData.name
                    checked: (plan.sheet.hiddenLayers || []).indexOf(modelData.uuid) < 0
                    onToggled: {
                        const hidden = (plan.sheet.hiddenLayers || []).filter(u => u !== modelData.uuid)
                        if (!checked)
                            hidden.push(modelData.uuid)
                        Session.setDrawing(plan.kind, "hiddenLayers", hidden)
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Export PDF…"); onClicked: { exportDialog.format = "pdf"; exportDialog.open() } }
            Button { text: qsTr("Export SVG…"); onClicked: { exportDialog.format = "svg"; exportDialog.open() } }
        }

        RowLayout {
            spacing: 6
            Label { text: qsTr("Title"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            Input {
                implicitWidth: 220
                text: plan.sheet.title || ""
                placeholderText: plan.defaultTitles[plan.kind]
                onEditingFinished: Session.setDrawing(plan.kind, "title", text.trim())
            }
            Check {
                text: qsTr("Auto dimensions")
                checked: plan.sheet.autoDimensions !== false
                onToggled: Session.setDrawing(plan.kind, "autoDimensions", checked)
            }
            Check {
                text: qsTr("Labels")
                checked: plan.sheet.labels !== false
                onToggled: Session.setDrawing(plan.kind, "labels", checked)
            }
            Check {
                text: qsTr("Legend")
                checked: plan.sheet.legend !== false
                onToggled: Session.setDrawing(plan.kind, "legend", checked)
            }
            Check {
                text: qsTr("Schedule")
                checked: plan.sheet.schedule === true
                onToggled: Session.setDrawing(plan.kind, "schedule", checked)
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Note")
                highlighted: sheetView.tool === "note"
                onClicked: { sheetView.tool = sheetView.tool === "note" ? "select" : "note"; sheetView.forceActiveFocus() }
            }
            Button {
                text: qsTr("Fit")
                enabled: sheetView.zoom !== 1
                onClicked: sheetView.fit()
            }
            Button {
                text: qsTr("Delete")
                enabled: sheetView.selectedDimension >= 0 || sheetView.selectedNote >= 0
                onClicked: sheetView.deleteSelected()
            }
            Button {
                text: qsTr("Clear dimensions")
                enabled: (plan.sheet.dimensions || []).length > 0
                onClicked: Session.setDrawing(plan.kind, "dimensions", [])
            }
        }

        DrawingItem {
            id: sheetView
            kind: plan.kind
            onEditNote: (index, note, itemPos) => {
                const p = mapToItem(plan, itemPos)
                noteEditor.index = index
                noteEditor.note = note
                noteEditor.x = Math.min(p.x, plan.width - 270)
                noteEditor.y = p.y + 8
                noteEditor.open()
            }
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Label {
            text: plan.message || (sheetView.tool === "note"
                                   ? qsTr("Click to place a note · drag from the target to add a leader · Esc cancels")
                                   : qsTr("Click to select · drag to move · Shift-drag dimension · N note · wheel zoom"))
            color: plan.failed ? Theme.err : Theme.muted
            font.pixelSize: Theme.small + 1
        }
    }
}
