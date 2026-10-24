import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// Build palette: drag a tile onto the 3D view to place it on the floor there, or click it to add at the
// origin. Draw walls / Draw deck switch the view to Top and turn clicks on the floor into points.
// shortcut: dropped objects land at their default height; parent to a truss under the cursor when asked.
ColumnLayout {
    id: root
    required property ViewportItem viewport
    property string tool: ""     // "", "walls" or "deck"
    property var last: null       // previous floor point [x, z] of the running tool
    property string error: ""
    signal wizard() // Main opens the New project wizard behind its unsaved-changes guard
    width: parent ? parent.width - 20 : 240
    x: 10
    spacing: 6

    // kind, label, fields; y is the default height a drop keeps.
    readonly property var tiles: [
        { kind: "deck", text: qsTr("Deck"), y: 0, fields: {} },
        { kind: "shape", text: qsTr("Box"), y: 0, fields: { shape: "box" } },
        { kind: "shape", text: qsTr("Cylinder"), y: 0, fields: { shape: "cylinder", name: "Cylinder" } },
        { kind: "shape", text: qsTr("Wedge"), y: 0, fields: { shape: "wedge", name: "Wedge" } },
        { kind: "shape", text: qsTr("Plane"), y: 0, fields: { shape: "box", name: "Plane", size: [2, 0.02, 2] } },
        { kind: "truss", text: qsTr("Truss"), y: 6, fields: {} },
        { kind: "wall", text: qsTr("LED wall"), y: 1, fields: {} },
        { kind: "strip", text: qsTr("Pixel strip"), y: 3, fields: {} }
    ].concat(Session.fixtureTypes.map(t => ({ kind: "fixture", y: 4,
        text: t.replace(/\.gdtf$/, "").replace(/^.*@/, "").replace(/_/g, " "),
        fields: { gdtf: t, mode: Session.modes(t)[0] } })))

    function place(tile, x, z) {
        error = Session.addObject(tile.kind, Object.assign({ position: [x, tile.y, z] }, tile.fields))
    }
    function start(name) {
        if (tool === name) return finish()
        tool = name
        last = null
        error = ""
        viewport.tool = "floor"
        viewport.setPreset("Top")
    }
    function finish() {
        tool = ""
        last = null
        viewport.tool = "select"
    }
    function num(field, fallback) {
        const v = parseFloat(field.text)
        return v > 0 ? v : fallback
    }

    Connections {
        target: root.viewport
        function onFloorClicked(p) {
            if (!root.tool) return
            const here = [p.x, p.z]
            if (root.last) {
                const a = root.last
                if (root.tool === "walls")
                    root.error = Session.addWallRun([a, here], root.num(wallH, 3), root.num(wallT, 0.2))
                else if (Math.abs(here[0] - a[0]) > 0.01 && Math.abs(here[1] - a[1]) > 0.01) {
                    root.error = Session.addObject("deck", { position: [(a[0] + here[0]) / 2, 0, (a[1] + here[1]) / 2],
                        width: Math.abs(here[0] - a[0]), depth: Math.abs(here[1] - a[1]), height: root.num(deckH, 1) })
                    root.last = null
                    return
                }
            }
            root.last = here
        }
    }

    GridLayout {
        columns: 3
        columnSpacing: 4
        rowSpacing: 4
        Layout.fillWidth: true
        Repeater {
            model: root.tiles
            Rectangle {
                id: tileItem
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                radius: 4
                color: drag.active || tap.pressed ? Theme.accentSoft : hover.hovered ? Theme.line : Theme.control
                border.color: Theme.line
                Label {
                    anchors.fill: parent
                    anchors.margins: 3
                    text: tileItem.modelData.text
                    color: Theme.fg
                    font.pixelSize: Theme.small
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                HoverHandler { id: hover; cursorShape: Qt.OpenHandCursor }
                TapHandler { id: tap; onTapped: root.place(tileItem.modelData, 0, 0) }
                DragHandler { id: drag; target: null }
                Drag.active: drag.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction
                Drag.mimeData: ({ "application/x-sv-palette": JSON.stringify(tileItem.modelData) })
                ToolTip.visible: hover.hovered && !drag.active
                ToolTip.delay: 600
                ToolTip.text: qsTr("Drag onto the view, or click to add at the centre")
            }
        }
    }

    Label {
        text: qsTr("DRAW")
        color: Theme.muted
        font.pixelSize: 8
        font.letterSpacing: 1.2
        Layout.topMargin: 4
    }
    RowLayout {
        spacing: 4
        HudButton { text: qsTr("Walls"); active: root.tool === "walls"; onClicked: root.start("walls") }
        Label { text: qsTr("h"); color: Theme.muted; font.pixelSize: Theme.small }
        Input { id: wallH; text: "3"; Layout.preferredWidth: 38; font.family: Theme.mono }
        Label { text: qsTr("t"); color: Theme.muted; font.pixelSize: Theme.small }
        Input { id: wallT; text: "0.2"; Layout.preferredWidth: 38; font.family: Theme.mono }
    }
    RowLayout {
        spacing: 4
        HudButton { text: qsTr("Deck"); active: root.tool === "deck"; onClicked: root.start("deck") }
        Label { text: qsTr("h"); color: Theme.muted; font.pixelSize: Theme.small }
        Input { id: deckH; text: "1"; Layout.preferredWidth: 38; font.family: Theme.mono }
    }
    Label {
        text: qsTr("CREATE")
        color: Theme.muted
        font.pixelSize: 8
        font.letterSpacing: 1.2
        Layout.topMargin: 4
    }
    Flow {
        Layout.fillWidth: true
        spacing: 4
        HudButton { text: qsTr("Fixture array…"); onClicked: arrayDialog.open() }
        HudButton { text: qsTr("Fixture type…"); onClicked: typeDialog.open() }
        HudButton { text: qsTr("New project…"); onClicked: root.wizard() }
    }
    ArrayDialog { id: arrayDialog }
    FixtureTypeDialog { id: typeDialog }
    Label {
        visible: root.tool !== "" || root.error !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: Theme.small
        color: root.error ? Theme.err : Theme.muted
        text: root.error ? root.error
            : root.tool === "walls"
              ? (root.last ? qsTr("Next corner (from %1, %2). Esc finishes.").arg(root.last[0]).arg(root.last[1])
                              : qsTr("Click the first corner on the floor."))
              : (root.last ? qsTr("Click the opposite corner.") : qsTr("Click one corner of the deck."))
    }
}
