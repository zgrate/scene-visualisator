import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SceneVisualiser

// FR-19 plan and elevation sheets: plan, front or side view of the rig with fixture symbols, automatic dimensions and
// a title block. Sheet size, scale and hidden layers are stored per view in project["drawings"]; Shift-drag on the
// sheet adds a dimension line. Exports the same drawing to PDF or SVG.
Rectangle {
    id: plan
    color: Theme.bg
    property string kind: "plan"
    readonly property var sheet: { Session.drawings; return Session.drawing(kind) } // re-read on every edit
    property string message
    property bool failed: false

    readonly property var scales: [0, 20, 25, 50, 100, 200]

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
            Button {
                text: qsTr("Clear dimensions")
                enabled: (plan.sheet.dimensions || []).length > 0
                onClicked: Session.setDrawing(plan.kind, "dimensions", [])
            }
            Button { text: qsTr("Export PDF…"); onClicked: { exportDialog.format = "pdf"; exportDialog.open() } }
            Button { text: qsTr("Export SVG…"); onClicked: { exportDialog.format = "svg"; exportDialog.open() } }
        }

        DrawingItem {
            kind: plan.kind
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Label {
            text: plan.message || qsTr("Shift-drag on the sheet to add a dimension line.")
            color: plan.failed ? Theme.err : Theme.muted
            font.pixelSize: Theme.small + 1
        }
    }
}
