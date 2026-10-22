import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// FR-13/14 composition source, opened from the dock: test pattern, mapping grid, still image, video file, screen or
// window capture. Status reads "1920×1080 · 59.9 fps" or "No signal" in the warn colour.
Popup {
    id: card
    width: 300
    padding: 12
    readonly property var source: (Session.compositionInfo.source || { kind: "testPattern" })
    readonly property var kinds: [["testPattern", qsTr("Test pattern")], ["grid", qsTr("Mapping grid")],
                                  ["still", qsTr("Still image")], ["video", qsTr("Video file")],
                                  ["screen", qsTr("Screen capture")], ["window", qsTr("Window capture")]]
    property string error

    function setSource(value) { card.error = ""; Session.edit("composition", "/source", value) }

    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }

    FileDialog {
        id: fileDialog
        property string kind
        nameFilters: kind === "still" ? [qsTr("Images (*.png *.jpg *.jpeg *.bmp *.webp)")]
                                      : [qsTr("Video (*.mp4 *.mov *.mkv *.avi *.webm *.m4v)"), qsTr("All files (*)")]
        onAccepted: card.error = Session.setMediaFile(kind, selectedFile)
    }

    ColumnLayout {
        width: parent.width
        spacing: 8
        Label {
            text: qsTr("COMPOSITION SOURCE")
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        ComboBox {
            Layout.fillWidth: true
            model: card.kinds.map(k => k[1])
            currentIndex: card.kinds.findIndex(k => k[0] === card.source.kind)
            onActivated: i => {
                const kind = card.kinds[i][0]
                if (kind === "still" || kind === "video") { fileDialog.kind = kind; fileDialog.open() }
                else if (kind === "screen" || kind === "window")
                    card.setSource({ kind: kind, name: Session.mediaSources(kind)[0] || "" })
                else card.setSource({ kind: kind })
            }
        }
        // Capture target
        ComboBox {
            id: target
            visible: card.source.kind === "screen" || card.source.kind === "window"
            Layout.fillWidth: true
            model: visible ? Session.mediaSources(card.source.kind) : []
            currentIndex: model.indexOf(card.source.name)
            displayText: currentIndex < 0 ? (card.source.name || qsTr("Nothing to capture")) : currentText
            onActivated: i => card.setSource({ kind: card.source.kind, name: model[i] })
            onPressedChanged: if (pressed) model = Session.mediaSources(card.source.kind) // re-list on open
        }
        // File
        RowLayout {
            visible: card.source.kind === "still" || card.source.kind === "video"
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: card.source.name || card.source.file || ""
                color: Theme.fg
                elide: Text.ElideMiddle
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
            }
            Button {
                text: qsTr("Choose…")
                onClicked: { fileDialog.kind = card.source.kind; fileDialog.open() }
            }
        }
        RowLayout {
            visible: card.source.kind === "still" || card.source.kind === "video"
            Label { text: qsTr("Fit"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            ComboBox {
                Layout.fillWidth: true
                model: ["fit", "fill", "stretch"]
                currentIndex: Math.max(0, model.indexOf(card.source.fit || "fit"))
                onActivated: i => Session.edit("composition", "/source/fit", model[i])
            }
        }
        // Playback
        RowLayout {
            visible: card.source.kind === "video"
            spacing: 8
            Button {
                text: card.source.playing === false ? qsTr("Play") : qsTr("Pause")
                onClicked: Session.edit("composition", "/source/playing", card.source.playing === false)
            }
            Check {
                text: qsTr("Loop")
                checked: card.source.loop !== false
                onToggled: Session.edit("composition", "/source/loop", checked)
            }
            Label { text: qsTr("Rate"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            Input {
                implicitWidth: 44
                text: String(card.source.rate || 1)
                font.family: Theme.mono
                validator: DoubleValidator { bottom: 0.1; top: 4 }
                onEditingFinished: Session.edit("composition", "/source/rate", Number(text))
            }
        }
        Label {
            readonly property var st: Session.mediaStatus
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: st.live || st.error !== ""
            text: st.error !== "" ? st.error
                  : st.signal ? qsTr("%1×%2 · %3 fps").arg(st.width).arg(st.height).arg(st.fps.toFixed(1))
                  : qsTr("No signal")
            color: st.error !== "" ? Theme.err : st.signal ? Theme.ok : Theme.warn
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
        }
        Label {
            visible: card.error !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.error
            color: Theme.err
            font.pixelSize: Theme.small + 1
        }
    }
}
