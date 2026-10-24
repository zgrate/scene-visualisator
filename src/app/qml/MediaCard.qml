import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// FR-13/14 composition source, opened from the dock: test pattern, mapping grid, still image, video file, screen or
// window capture, NDI, Spout (Windows) or Syphon (macOS). Status reads "1920×1080 · 59.9 fps · 18 ms" (NFR-3:
// arrival to render) or "No signal" in the warn colour.
Popup {
    id: card
    width: 340
    margins: 8 // kept inside the window when the dock button sits near its edge
    padding: 12
    readonly property var source: (Session.compositionInfo.source || { kind: "testPattern" })
    readonly property var kinds: [["testPattern", qsTr("Test pattern")], ["grid", qsTr("Mapping grid")],
                                  ["still", qsTr("Still image")], ["video", qsTr("Video file")],
                                  ["screen", qsTr("Screen capture")], ["window", qsTr("Window capture")],
                                  ["ndi", qsTr("NDI")]].concat(
                                  Qt.platform.os === "windows" ? [["spout", qsTr("Spout")]]
                                  : Qt.platform.os === "osx" ? [["syphon", qsTr("Syphon")]] : [])
    readonly property bool named: ["screen", "window", "ndi", "spout", "syphon"].includes(card.source.kind)
    // NDI discovery blocks for a second and runs only on request (NFR-9), so it is never part of a binding.
    function listed(kind) {
        return kind === "ndi" ? (card.source.name ? [card.source.name] : []) : Session.mediaSources(kind)
    }
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
                else if (["screen", "window", "ndi", "spout", "syphon"].includes(kind))
                    card.setSource({ kind: kind, name: Session.mediaSources(kind)[0] || "" })
                else card.setSource({ kind: kind })
            }
        }
        // Capture target or sender
        ComboBox {
            id: target
            visible: card.named
            Layout.fillWidth: true
            model: visible ? card.listed(card.source.kind) : []
            currentIndex: model.indexOf(card.source.name)
            displayText: currentIndex < 0 ? (card.source.name || qsTr("Nothing found")) : currentText
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
                  : st.signal ? qsTr("%1×%2 · %3 fps · %4 ms").arg(st.width).arg(st.height).arg(st.fps.toFixed(1))
                                                                .arg(st.latency.toFixed(0))
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

        // One feed per Resolume output (Arena sends an NDI stream per Screen); its walls show their output rect of
        // it. Lumiverse outputs come in over Art-Net/sACN and only show their state.
        RowLayout {
            visible: Session.feedStatus.length > 0
            Layout.topMargin: 6
            Label {
                Layout.fillWidth: true
                text: qsTr("RESOLUME OUTPUTS")
                color: Theme.muted
                font.pixelSize: 8
                font.letterSpacing: 1.2
            }
            Button {
                text: qsTr("Connect NDI")
                onClicked: card.error = Session.connectResolumeFeeds()
            }
        }
        Repeater {
            model: Session.feedStatus.length // a count, so the 20 Hz status refresh keeps the delegates (and open combos)
            ColumnLayout {
                id: row
                required property int index
                readonly property var st: Session.feedStatus[index] || ({})
                readonly property bool dmx: st.kind === "lumiverse"
                readonly property string feedKind: st.feedKind || ""
                Layout.fillWidth: true
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    Rectangle {
                        implicitWidth: 6; implicitHeight: 6; radius: 3
                        color: row.st.error ? Theme.err : row.st.signal ? Theme.ok
                               : row.dmx || row.feedKind !== "" ? Theme.warn : Theme.line
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.st.name || ""
                        color: Theme.fg
                        elide: Text.ElideRight
                        font.pixelSize: Theme.small + 1
                    }
                    Label {
                        text: row.dmx ? qsTr("U%1 · %2 Hz").arg(row.st.universe).arg((row.st.rate || 0).toFixed(0))
                              : row.st.error ? row.st.error
                              : row.st.signal ? qsTr("%1 fps · %2 ms").arg(row.st.fps.toFixed(1)).arg(row.st.latency.toFixed(0))
                              : row.feedKind !== "" ? qsTr("No signal") : qsTr("%1×%2").arg(row.st.width).arg(row.st.height)
                        color: row.st.error ? Theme.err : Theme.muted
                        elide: Text.ElideRight
                        Layout.maximumWidth: 150
                        font.family: Theme.mono
                        font.pixelSize: Theme.small
                    }
                }
                RowLayout {
                    visible: !row.dmx
                    Layout.fillWidth: true
                    readonly property var feedKinds: [["", qsTr("Composition")], ["ndi", qsTr("NDI")],
                                                      ["screen", qsTr("Screen")], ["window", qsTr("Window")]].concat(
                                                      Qt.platform.os === "windows" ? [["spout", qsTr("Spout")]]
                                                      : Qt.platform.os === "osx" ? [["syphon", qsTr("Syphon")]] : [])
                    ComboBox {
                        id: feedKind
                        implicitWidth: 104
                        model: parent.feedKinds.map(k => k[1])
                        currentIndex: Math.max(0, parent.feedKinds.findIndex(k => k[0] === row.feedKind))
                        onActivated: i => {
                            const kind = parent.feedKinds[i][0]
                            card.error = ""
                            Session.edit(row.st.uuid, "/feed", kind === "" ? null
                                         : { kind: kind, name: kind === "ndi" ? "" : (Session.mediaSources(kind)[0] || "") })
                        }
                    }
                    ComboBox {
                        visible: row.feedKind !== ""
                        Layout.fillWidth: true
                        model: row.st.feedName ? [row.st.feedName] : [] // listed on open: NDI discovery blocks
                        currentIndex: model.indexOf(row.st.feedName)
                        displayText: row.st.feedName || qsTr("Pick a sender…")
                        onActivated: i => Session.edit(row.st.uuid, "/feed", { kind: row.feedKind, name: model[i] })
                        onPressedChanged: if (pressed) model = Session.mediaSources(row.feedKind)
                    }
                }
            }
        }
    }
}
