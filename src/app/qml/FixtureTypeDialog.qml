import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// FR-4: build a simple fixture type (one mode, fixed Base > Yoke > Head > Beam geometry) or rework a library type.
Dialog {
    id: dlg
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Fixture type")
    property string error: ""
    property var warnings: []
    property var channels: [] // [{attribute, bits, default, physicalFrom?, physicalTo?}]
    readonly property var attributes: ["Dimmer", "Pan", "Tilt", "Zoom", "Focus", "Iris", "Frost",
        "ColorAdd_R", "ColorAdd_G", "ColorAdd_B", "ColorAdd_W", "ColorSub_C", "ColorSub_M", "ColorSub_Y",
        "CTO", "Color1", "Gobo1", "Shutter1", "Shutter1Strobe"]

    onAboutToShow: { source.currentIndex = 0; load("") }

    function load(gdtf) {
        error = ""
        const s = gdtf ? Session.fixtureSpec(gdtf) : null
        name.text = s ? s.name + (s.bundled ? " copy" : "") : ""
        maker.text = s ? s.manufacturer : ""
        shortName.text = s ? s.shortName : ""
        beam.text = s ? s.beamAngle : 25
        field.text = s ? s.fieldAngle : 30
        lumens.text = s ? s.lumens : 10000
        bl.text = s ? s.body[0] : 0.3
        bh.text = s ? s.body[1] : 0.4
        bw.text = s ? s.body[2] : 0.3
        emitter.text = s ? s.emitter : "#ffffff"
        warnings = s ? s.warnings : []
        channels = s ? s.channels : [{ attribute: "Dimmer", bits: 8, default: 0 }]
    }
    function setChannel(i, key, value) {
        const c = channels.slice()
        c[i] = Object.assign({}, c[i], { [key]: value })
        if (key === "attribute") { delete c[i].physicalFrom; delete c[i].physicalTo } // new attribute, its defaults
        channels = c
    }
    function num(f, fallback) { const v = parseFloat(f.text); return isNaN(v) ? fallback : v }
    function save() {
        error = Session.saveFixtureType({
            name: name.text, manufacturer: maker.text, shortName: shortName.text,
            beamAngle: num(beam, 25), fieldAngle: num(field, 30), lumens: num(lumens, 10000),
            body: [num(bl, 0.3), num(bh, 0.4), num(bw, 0.3)], emitter: emitter.text, channels: channels
        })
        if (!error) close()
    }

    ColumnLayout {
        spacing: 8
        component Key: Label { color: Theme.muted; font.pixelSize: Theme.small + 1 }
        component Num: Input { Layout.preferredWidth: 60; font.family: Theme.mono }

        RowLayout {
            Key { text: qsTr("Start from") }
            ComboBox {
                id: source
                Layout.preferredWidth: 280
                model: [qsTr("New type")].concat(Session.fixtureTypes)
                onActivated: dlg.load(currentIndex > 0 ? currentText : "")
            }
        }
        Label {
            visible: dlg.warnings.length > 0
            Layout.maximumWidth: 420
            wrapMode: Text.Wrap
            font.pixelSize: Theme.small + 1
            color: Theme.warn
            text: dlg.warnings.join(". ") + ". " + qsTr("Saving writes a simple type: pixel layouts, wheel images and 3D models are dropped.")
        }
        GridLayout {
            columns: 4
            columnSpacing: 10
            rowSpacing: 6
            Key { text: qsTr("Name") }
            Input { id: name; Layout.preferredWidth: 150 }
            Key { text: qsTr("Manufacturer") }
            Input { id: maker; Layout.preferredWidth: 120 }
            Key { text: qsTr("Short name") }
            Input { id: shortName; Layout.preferredWidth: 150 }
            Key { text: qsTr("Emitter") }
            RowLayout {
                Input { id: emitter; Layout.preferredWidth: 80; font.family: Theme.mono }
                Rectangle { width: 16; height: 16; radius: 3; color: emitter.text; border.color: Theme.line }
            }
            Key { text: qsTr("Beam / field °") }
            RowLayout { Num { id: beam } Num { id: field } }
            Key { text: qsTr("Lumens") }
            Num { id: lumens; Layout.preferredWidth: 80 }
            Key { text: qsTr("Body L H W m") }
            RowLayout { Layout.columnSpan: 3; Num { id: bl } Num { id: bh } Num { id: bw } }
        }
        Label { text: qsTr("CHANNELS"); color: Theme.muted; font.pixelSize: 8; font.letterSpacing: 1.2 }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(rows.implicitHeight, 260)
            clip: true
            ColumnLayout {
                id: rows
                spacing: 4
                Repeater {
                    model: dlg.channels
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        Label { text: index + 1; Layout.preferredWidth: 20; color: Theme.muted; font.family: Theme.mono }
                        ComboBox {
                            Layout.preferredWidth: 160
                            model: dlg.attributes
                            currentIndex: dlg.attributes.indexOf(modelData.attribute)
                            onActivated: dlg.setChannel(index, "attribute", currentText)
                        }
                        ComboBox {
                            Layout.preferredWidth: 80
                            model: ["8 bit", "16 bit"]
                            currentIndex: modelData.bits === 16 ? 1 : 0
                            onActivated: dlg.setChannel(index, "bits", currentIndex ? 16 : 8)
                        }
                        Key { text: qsTr("default") }
                        Input {
                            Layout.preferredWidth: 60
                            font.family: Theme.mono
                            text: modelData.default
                            validator: IntValidator { bottom: 0; top: 65535 }
                            onEditingFinished: dlg.setChannel(index, "default", parseInt(text) || 0)
                        }
                        HudButton {
                            text: "✕"
                            onClicked: { const c = dlg.channels.slice(); c.splice(index, 1); dlg.channels = c }
                        }
                    }
                }
            }
        }
        HudButton {
            text: qsTr("+ channel")
            onClicked: dlg.channels = dlg.channels.concat([{ attribute: "Dimmer", bits: 8, default: 0 }])
        }
        Label {
            visible: dlg.error !== ""
            Layout.maximumWidth: 420
            wrapMode: Text.Wrap
            font.pixelSize: Theme.small + 1
            color: Theme.err
            text: dlg.error
        }
    }
    footer: DialogButtonBox {
        Button { text: qsTr("Save type"); onClicked: dlg.save() }
        Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
}
