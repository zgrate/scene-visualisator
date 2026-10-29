import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Show recording, opened from the dock's REC button: records the merged DMX (.svdmx) and, while a live media source
// runs, the composition picture (.mp4 beside it); plays both back, looping. While playing, SOURCES picks per default,
// output, group or fixture whether the take, live input or both (HTP) drive it, each with
// its own fader under the master.
Popup {
    id: card
    signal progress() // open the render progress screen
    width: 340
    padding: 12
    readonly property var rec: Session.recording
    // Default, outputs, groups, then fixtures that carry their own mode or fader. Text, so the 20 Hz status refresh only
    // rebuilds the rows when they really change.
    readonly property string scopeRows: {
        const scopes = card.rec.scopes || {}, fades = card.rec.fades || {}
        const rows = [{ key: "default", name: qsTr("Everything else"), kind: "" }]
        Session.feedStatus.forEach(o => rows.push({ key: o.uuid, name: o.name,
                                                    kind: o.kind === "lumiverse" ? "DMX" : qsTr("screen") }))
        Session.controlGroups.forEach(g => rows.push({ key: g.key, name: g.name, kind: g.kind }))
        const known = new Set(rows.map(r => r.key))
        Session.objects.forEach(o => { if ((scopes[o.uuid] || o.uuid in fades) && !known.has(o.uuid))
                                           rows.push({ key: o.uuid, name: o.name, kind: qsTr("fixture") }) })
        return JSON.stringify(rows)
    }
    property string error

    function clock(ms) {
        const s = Math.floor(ms / 1000)
        const two = n => (n < 10 ? "0" : "") + n
        return (s >= 3600 ? Math.floor(s / 3600) + ":" : "") + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
    }

    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }

    FileDialog {
        id: recordDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "svdmx"
        nameFilters: [qsTr("Show recordings (*.svdmx)")]
        onAccepted: card.error = Session.startRecording(selectedFile, quality.currentIndex === 1, viewCheck.checked)
    }
    FileDialog {
        id: renderDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: [qsTr("Video (*.mp4)")]
        onAccepted: {
            const p = renderPreset.presets[renderPreset.currentIndex]
            card.error = Session.startRender({ path: selectedFile, fps: Number(renderFps.text) || 30,
                                               lossless: quality.currentIndex === 1,
                                               quality: p[0], supersample: p[1], samples: p[2] })
        }
    }
    FileDialog {
        id: playDialog
        nameFilters: [qsTr("Show recordings (*.svdmx)")]
        onAccepted: card.error = Session.startPlayback(selectedFile)
    }

    ColumnLayout {
        width: parent.width
        spacing: 8
        Label {
            text: qsTr("SHOW RECORDING")
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        RowLayout {
            Label { text: qsTr("Video"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            ComboBox {
                id: quality
                Layout.fillWidth: true
                enabled: !card.rec.active
                model: [qsTr("Compressed · ~2 GB/h"), qsTr("Near-lossless · ~60 GB/h")]
                currentIndex: String(Session.preference("recordLossless", false)) === "true" ? 1 : 0
            }
        }
        Check {
            id: viewCheck
            text: qsTr("Also record the 3D view (.view.mp4)")
            enabled: !card.rec.active
            checked: String(Session.preference("recordView", false)) === "true"
        }
        RowLayout {
            spacing: 8
            Button {
                text: card.rec.active ? qsTr("Stop recording") : qsTr("Record…")
                enabled: !card.rec.playing
                palette.buttonText: card.rec.active ? Theme.err : Theme.fg
                onClicked: {
                    card.error = ""
                    if (card.rec.active) Session.stopRecording()
                    else {
                        // A take is named by when it starts; the name stays editable in the dialog.
                        const folder = recordDialog.currentFolder.toString()
                                       || StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                        recordDialog.selectedFile = folder + "/" + Qt.formatDateTime(new Date(), "yyyy-MM-dd_HH-mm-ss")
                                                    + ".svdmx"
                        recordDialog.open()
                    }
                }
            }
            Button {
                text: qsTr("Play…")
                enabled: !card.rec.active
                onClicked: { card.error = ""; playDialog.open() }
            }
        }
        // Playback transport
        RowLayout {
            visible: card.rec.playing
            spacing: 8
            Button {
                text: card.rec.paused ? qsTr("Play") : qsTr("Pause")
                onClicked: Session.pausePlayback(!card.rec.paused)
            }
            Button {
                text: qsTr("Stop")
                onClicked: Session.stopPlayback()
            }
        }
        // Art-Net timecode chase: the take runs at the incoming timecode minus the offset
        RowLayout {
            visible: card.rec.playing
            spacing: 8
            Check {
                id: chase
                text: qsTr("Chase TC")
                checked: card.rec.chase === true
                onToggled: card.error = Session.setPlaybackChase(checked, chaseOffset.text)
            }
            Input {
                id: chaseOffset
                Layout.preferredWidth: 96
                placeholderText: "01:00:00:00"
                font.family: Theme.mono
                onAccepted: card.error = Session.setPlaybackChase(chase.checked, text)
            }
            Label {
                Layout.fillWidth: true
                text: card.rec.timecode ? card.rec.timecode : qsTr("no timecode")
                color: card.rec.timecode ? Theme.ok : Theme.muted
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
            }
        }
        Slider {
            visible: card.rec.playing
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, card.rec.duration)
            value: pressed ? value : card.rec.t
            onMoved: Session.seekPlayback(value)
        }
        Label {
            visible: card.rec.active || card.rec.playing
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            text: (card.rec.active ? "● " + card.clock(card.rec.t)
                                   : card.clock(card.rec.t) + " / " + card.clock(card.rec.duration))
                  + (card.rec.hasVideo ? " · " + qsTr("DMX + video") : " · " + qsTr("DMX only"))
                  + " · " + card.rec.path.split(/[\/\\]/).pop()
            color: card.rec.active ? Theme.err : Theme.fg
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
        }
        // Crossfader: live input .. the take, as the scopes below allow
        RowLayout {
            visible: card.rec.playing
            Layout.fillWidth: true
            Label { text: qsTr("MASTER  LIVE"); color: Theme.muted; font.pixelSize: Theme.small }
            Slider {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: pressed ? value : card.rec.fade
                onMoved: Session.setPlaybackFade(value)
            }
            Label { text: qsTr("REC"); color: Theme.muted; font.pixelSize: Theme.small }
        }
        Label {
            readonly property var out: Session.outputStatus
            visible: card.rec.playing && out.enabled
            text: qsTr("OUT ● %1 %2 · %3 universes").arg(out.protocol === "sacn" ? "sACN" : "Art-Net")
                  .arg(out.target || qsTr("broadcast")).arg(out.universes)
            color: out.error ? Theme.err : Theme.accent
            font.family: Theme.mono
            font.pixelSize: Theme.small
        }
        RowLayout {
            visible: card.rec.playing
            Layout.topMargin: 4
            Label {
                Layout.fillWidth: true
                text: qsTr("SOURCES")
                color: Theme.muted
                font.pixelSize: 8
                font.letterSpacing: 1.2
            }
            Button {
                text: qsTr("Add selection")
                enabled: Session.selectedUuids.length > 0
                onClicked: Session.selectedUuids.forEach(u => Session.setPlaybackScope(u, "live"))
            }
        }
        ScrollView {
            visible: card.rec.playing
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(240, scopeColumn.implicitHeight)
            clip: true
            ColumnLayout {
                id: scopeColumn
                width: card.availableWidth
                spacing: 2
                Repeater {
                    model: JSON.parse(card.scopeRows)
                    ColumnLayout {
                        id: scopeRow
                        required property var modelData
                        readonly property string mode: (card.rec.scopes || {})[modelData.key] || ""
                        readonly property var ownFade: (card.rec.fades || {})[modelData.key]
                        readonly property bool hasFade: ownFade !== undefined
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                Layout.fillWidth: true
                                text: scopeRow.modelData.name
                                color: scopeRow.mode !== "" ? Theme.fg : Theme.muted
                                elide: Text.ElideRight
                                font.pixelSize: Theme.small + 1
                            }
                            Label {
                                text: scopeRow.modelData.kind
                                color: Theme.muted
                                font.pixelSize: Theme.small
                            }
                            Repeater {
                                model: [["live", qsTr("Live")], ["recording", qsTr("Rec")], ["both", qsTr("Both")]]
                                Rectangle {
                                    required property var modelData
                                    readonly property bool on: scopeRow.mode === modelData[0]
                                    implicitWidth: 34
                                    implicitHeight: 18
                                    radius: 3
                                    color: on ? Theme.accentSoft : "transparent"
                                    border.color: on ? Theme.accent : Theme.line
                                    Label {
                                        anchors.centerIn: parent
                                        text: parent.modelData[1]
                                        color: parent.on ? Theme.accent : Theme.muted
                                        font.pixelSize: Theme.small
                                    }
                                    TapHandler { // a second tap drops the override (the default keeps one)
                                        onTapped: Session.setPlaybackScope(scopeRow.modelData.key,
                                                                           parent.on ? "" : parent.modelData[0])
                                    }
                                }
                            }
                        }
                        // The part's own fader, under the master; inherited (muted) until moved
                        RowLayout {
                            visible: scopeRow.hasFade
                                     || (scopeRow.mode || (card.rec.scopes || {})["default"]) !== "live"
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            spacing: 4
                            Slider {
                                Layout.fillWidth: true
                                implicitHeight: 20
                                from: 0
                                to: 1
                                value: pressed ? value : scopeRow.hasFade ? scopeRow.ownFade
                                                                          : ((card.rec.fades || {})["default"] ?? 1)
                                onMoved: Session.setPlaybackScopeFade(scopeRow.modelData.key, value)
                            }
                            Label {
                                Layout.preferredWidth: 34
                                horizontalAlignment: Text.AlignRight
                                text: Math.round(100 * (scopeRow.hasFade ? scopeRow.ownFade
                                                        : ((card.rec.fades || {})["default"] ?? 1))) + "%"
                                color: scopeRow.hasFade ? Theme.accent : Theme.muted
                                font.family: Theme.mono
                                font.pixelSize: Theme.small
                                TapHandler { // drops the part's own fader
                                    enabled: scopeRow.hasFade && scopeRow.modelData.key !== "default"
                                    onTapped: Session.setPlaybackScopeFade(scopeRow.modelData.key, -1)
                                }
                            }
                        }
                    }
                }
            }
        }
        // Offline render: the take or the composition video, frame by frame from the current camera, at 1920×1080
        Label {
            text: qsTr("RENDER")
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        RowLayout {
            spacing: 8
            readonly property var job: card.rec.render || {}
            Input {
                id: renderFps
                Layout.preferredWidth: 40
                enabled: !parent.job.active
                text: "30"
                font.family: Theme.mono
                validator: IntValidator { bottom: 1; top: 240 }
            }
            Label { text: qsTr("fps"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            ComboBox {
                id: renderPreset
                Layout.preferredWidth: 150
                enabled: !parent.job.active
                // quality, supersample, samples (0 = the quality's default)
                readonly property var presets: [["cinematic", true, 0], ["cinematic", false, 0],
                                                ["cinematic", false, 2], ["high", true, 1]]
                model: [qsTr("Cinematic · 2× · 4 samples"), qsTr("Cinematic · 1× · 4 samples"),
                        qsTr("Draft · 1× · 2 samples"), qsTr("Viewport look")]
                currentIndex: Math.min(3, Math.max(0, Number(Session.preference("renderPreset", 0))))
                onActivated: i => Session.setPreference("renderPreset", i)
            }
            Button {
                text: parent.job.active ? qsTr("Stop") : qsTr("Render…")
                enabled: parent.job.active || !card.rec.active
                palette.buttonText: parent.job.active ? Theme.err : Theme.fg
                onClicked: {
                    card.error = ""
                    if (parent.job.active) Session.stopRender()
                    else renderDialog.open()
                }
            }
            Label {
                Layout.fillWidth: true
                visible: parent.job.frames > 0
                text: qsTr("frame %1 / %2").arg(parent.job.frame).arg(parent.job.frames)
                color: parent.job.active ? Theme.accent : Theme.muted
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
            }
            Button {
                visible: parent.job.frames > 0
                text: qsTr("Progress")
                onClicked: { card.close(); card.progress() }
            }
        }
        Label {
            visible: card.error !== "" || card.rec.error !== "" || !!(card.rec.render || {}).error
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.error !== "" ? card.error : card.rec.error || (card.rec.render || {}).error
            color: Theme.err
            font.pixelSize: Theme.small + 1
        }
    }
}
