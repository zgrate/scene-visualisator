import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// A new project built in one pass: venue, deck, trusses with patched fixtures, back and side walls, pixel strips.
Dialog {
    id: dlg
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("New project")
    property string error: ""
    signal built()
    onAboutToShow: {
        error = ""
        const i = Session.fixtureTypes.indexOf("Generic@Spot_7ch.gdtf")
        type.currentIndex = i >= 0 ? i : 0
    }

    function num(f, fallback) { const v = parseFloat(f.text); return isNaN(v) ? fallback : v }
    function build() {
        const errors = []
        const check = e => { if (e) errors.push(e) }
        Session.newProject()
        const W = num(vw, 20), D = num(vd, 15)
        Session.edit("venue", "/width", W)
        Session.edit("venue", "/depth", D)
        Session.edit("venue", "/height", num(vh, 8))
        const dw = num(dkw, 10), dd = num(dkd, 6), dh = deck.checked ? num(dkh, 1) : 0
        if (deck.checked)
            check(Session.addObject("deck", { name: qsTr("Deck"), position: [0, 0, 0], width: dw, depth: dd, height: dh }))
        const nt = Math.round(num(tc, 0)), L = num(tl, 10), gap = num(tz, 3), n = Math.round(num(fc, 0))
        for (let i = 0; i < nt; ++i) {
            const z = (i - (nt - 1) / 2) * gap
            check(Session.addObject("truss", { name: qsTr("Truss %1").arg(i + 1), position: [0, num(th, 6.5), z], length: L }))
            if (n > 0 && type.currentText)
                check(Session.addFixtureArray(type.currentText, mode.currentText, n, [-L / 2 + L / (2 * n), -0.2, 0],
                                              [L / n, 0, 0], "", Session.current.uuid))
        }
        if (walls.checked)
            check(Session.addWallRun([[-W / 2, D / 2], [-W / 2, -D / 2], [W / 2, -D / 2], [W / 2, D / 2]],
                                     num(wh, 3), num(wt, 0.2)))
        const ns = Math.round(num(sc, 0)), px = Math.round(num(sp, 60))
        for (let i = 0; i < ns; ++i) // along the deck's front edge, one slot each
            check(Session.addObject("strip", { name: qsTr("Strip %1").arg(i + 1), pixels: px, length: px / 60,
                                               position: [(i - (ns - 1) / 2) * dw / ns, dh + 0.02, dd / 2] }))
        Session.selectUuids([])
        error = errors.join("\n")
        if (!error) { close(); built() }
    }

    GridLayout {
        columns: 4
        columnSpacing: 10
        rowSpacing: 6
        component Key: Label { color: Theme.muted; font.pixelSize: Theme.small + 1 }
        component Head: Label {
            Layout.columnSpan: 4
            Layout.topMargin: 6
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        component Num: Input { Layout.preferredWidth: 56; font.family: Theme.mono }
        component Three: RowLayout { spacing: 4; Layout.columnSpan: 3 }

        Head { text: qsTr("VENUE") }
        Key { text: qsTr("W D H m") }
        Three { Num { id: vw; text: "20" } Num { id: vd; text: "15" } Num { id: vh; text: "8" } }

        Head { text: qsTr("DECK") }
        Check { id: deck; text: qsTr("Stage deck"); checked: true; Layout.columnSpan: 4 }
        Key { text: qsTr("W D H m"); enabled: deck.checked }
        Three { enabled: deck.checked; Num { id: dkw; text: "10" } Num { id: dkd; text: "6" } Num { id: dkh; text: "1" } }

        Head { text: qsTr("TRUSSES AND FIXTURES") }
        Key { text: qsTr("Trusses") }
        Num { id: tc; text: "2"; validator: IntValidator { bottom: 0; top: 20 } }
        Key { text: qsTr("Length m") }
        Num { id: tl; text: "10" }
        Key { text: qsTr("Height m") }
        Num { id: th; text: "6.5" }
        Key { text: qsTr("Spacing m") }
        Num { id: tz; text: "3" }
        Key { text: qsTr("Type") }
        ComboBox { id: type; Layout.columnSpan: 3; Layout.preferredWidth: 240; model: Session.fixtureTypes }
        Key { text: qsTr("Mode") }
        ComboBox { id: mode; Layout.preferredWidth: 140; model: type.currentText ? Session.modes(type.currentText) : [] }
        Key { text: qsTr("Per truss") }
        Num { id: fc; text: "6"; validator: IntValidator { bottom: 0; top: 64 } }

        Head { text: qsTr("WALLS") }
        Check { id: walls; text: qsTr("Back and side walls at the venue edge"); Layout.columnSpan: 4 }
        Key { text: qsTr("Height m"); enabled: walls.checked }
        Num { id: wh; text: "3"; enabled: walls.checked }
        Key { text: qsTr("Thickness m"); enabled: walls.checked }
        Num { id: wt; text: "0.2"; enabled: walls.checked }

        Head { text: qsTr("PIXEL STRIPS") }
        Key { text: qsTr("Strips") }
        Num { id: sc; text: "0"; validator: IntValidator { bottom: 0; top: 64 } }
        Key { text: qsTr("Pixels each") }
        Num { id: sp; text: "60"; validator: IntValidator { bottom: 1; top: 1024 } }

        Label {
            visible: dlg.error !== ""
            Layout.columnSpan: 4
            Layout.maximumWidth: 380
            wrapMode: Text.Wrap
            font.pixelSize: Theme.small + 1
            color: Theme.err
            text: dlg.error
        }
    }
    footer: DialogButtonBox {
        Button { text: qsTr("Create"); onClicked: dlg.build() }
        Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
}
