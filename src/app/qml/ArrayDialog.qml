import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// FR-4 FR-8: place a row of patched fixtures, either free on a line or spread along the selected truss.
Dialog {
    id: dlg
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Fixture array")
    property string error: ""
    readonly property bool onTruss: layout.currentIndex === 1
    readonly property real trussLength: Session.current.type === "truss" ? Session.current.length || 0 : 0
    onAboutToShow: {
        error = ""
        layout.currentIndex = trussLength > 0 ? 1 : 0
    }

    function num(field, fallback) {
        const v = parseFloat(field.text)
        return isNaN(v) ? fallback : v
    }
    function accept_() {
        const n = Math.round(num(count, 0))
        let origin, step, parent = ""
        if (onTruss) {
            const L = trussLength
            origin = [-L / 2 + L / (2 * n), num(hang, -0.2), 0]
            step = [L / n, 0, 0]
            parent = Session.current.uuid
        } else {
            const d = num(spacing, 1)
            origin = [num(sx, 0) - d * (n - 1) / 2, num(sy, 4), num(sz, 0)]
            step = [d, 0, 0]
        }
        error = Session.addFixtureArray(type.currentText, mode.currentText, n, origin, step, patch.text, parent)
        if (!error) close()
    }

    GridLayout {
        columns: 2
        columnSpacing: 10
        rowSpacing: 6
        component Key: Label { color: Theme.muted; font.pixelSize: Theme.small + 1 }

        Key { text: qsTr("Type") }
        ComboBox { id: type; Layout.preferredWidth: 240; model: Session.fixtureTypes }
        Key { text: qsTr("Mode") }
        ComboBox { id: mode; Layout.preferredWidth: 240; model: type.currentText ? Session.modes(type.currentText) : [] }
        Key { text: qsTr("Count") }
        Input { id: count; text: "6"; Layout.preferredWidth: 60; font.family: Theme.mono; validator: IntValidator { bottom: 1; top: 512 } }
        Key { text: qsTr("Layout") }
        ComboBox {
            id: layout
            Layout.preferredWidth: 240
            model: [qsTr("Row"), qsTr("Along selected truss")]
        }
        Key { text: qsTr("Spacing m"); visible: !dlg.onTruss }
        Input { id: spacing; text: "1"; visible: !dlg.onTruss; Layout.preferredWidth: 60; font.family: Theme.mono }
        Key { text: qsTr("Centre x y z"); visible: !dlg.onTruss }
        RowLayout {
            visible: !dlg.onTruss
            Input { id: sx; text: "0"; Layout.preferredWidth: 60; font.family: Theme.mono }
            Input { id: sy; text: "4"; Layout.preferredWidth: 60; font.family: Theme.mono }
            Input { id: sz; text: "0"; Layout.preferredWidth: 60; font.family: Theme.mono }
        }
        Key { text: qsTr("Hang m"); visible: dlg.onTruss }
        Input { id: hang; text: "-0.2"; visible: dlg.onTruss; Layout.preferredWidth: 60; font.family: Theme.mono }
        Key { text: qsTr("Start patch") }
        Input { id: patch; placeholderText: qsTr("next free, or 2.001"); Layout.preferredWidth: 140; font.family: Theme.mono }
        Label {
            Layout.columnSpan: 2
            Layout.maximumWidth: 320
            wrapMode: Text.Wrap
            font.pixelSize: Theme.small + 1
            color: dlg.error ? Theme.err : Theme.muted
            text: dlg.error ? dlg.error
                : dlg.onTruss ? (dlg.trussLength > 0 ? qsTr("Spread evenly along “%1” (%2 m), parented to it.")
                                                        .arg(Session.current.name).arg(dlg.trussLength)
                                                     : qsTr("Select a truss first."))
                : qsTr("A row along X, centred on the point.")
        }
    }
    footer: DialogButtonBox {
        Button { text: qsTr("Add"); enabled: !dlg.onTruss || dlg.trussLength > 0; onClicked: dlg.accept_() }
        Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
}
