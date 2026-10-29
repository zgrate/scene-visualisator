import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Offline render progress (section 26): rate and ETA, where each frame's time goes (the widest band is the
// bottleneck), the device and encoder, and the last rendered frame. Covers the stage; the viewport rests meanwhile.
Rectangle {
    id: screen
    color: Theme.bg
    signal closed()
    // Hidden, the screen reads nothing from Session, so its bindings cost the render no UI time.
    readonly property var job: visible ? (Session.recording.render || {}) : ({})
    readonly property var stages: job.stages || {}
    readonly property var rows: [["waitVideo", qsTr("wait video"), Theme.sources[0]],
                                 ["waitDmx", qsTr("wait DMX"), Theme.sources[1]],
                                 ["waitEncoder", qsTr("wait encoder"), Theme.err],
                                 ["build", qsTr("scene build"), Theme.sources[2]],
                                 ["draw", qsTr("GPU draw + sync"), Theme.accent],
                                 ["readback", qsTr("readback"), Theme.sources[3]],
                                 ["push", qsTr("encode push"), Theme.warn],
                                 ["other", qsTr("other (UI %1 ms/tick)").arg((job.uiMs || 0).toFixed(1)), Theme.muted]]
    readonly property real total: rows.reduce((sum, r) => sum + (stages[r[0]] || 0), 0)
    function clock(ms) {
        const s = Math.max(0, Math.round(ms / 1000))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    function fileName(path) { return (path || "").split(/[\\/]/).pop() }

    // Clicks and wheel stop here instead of reaching the viewport below.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.gap, 640)
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: screen.fileName(screen.job.path) || qsTr("Render")
                color: Theme.fg
                font.pixelSize: Theme.fontSize + 2
                elide: Text.ElideMiddle
            }
            Button {
                visible: !!screen.job.active
                text: qsTr("Stop")
                palette.buttonText: Theme.err
                onClicked: Session.stopRender()
            }
            Button { text: screen.job.active ? qsTr("Hide") : qsTr("Close"); onClicked: screen.closed() }
        }
        Label {
            text: "%1×%2 · %3 fps · %4 · %5 %6".arg(screen.job.width || 0).arg(screen.job.height || 0)
                  .arg(screen.job.fps || 0).arg(screen.job.quality || "").arg(screen.job.samples || 1)
                  .arg((screen.job.samples || 1) > 1 ? qsTr("samples") : qsTr("sample"))
                  + (screen.job.quality === "cinematic" && screen.job.supersample ? " · 2×" : "")
            color: Theme.muted
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 4
            height: 8
            radius: 4
            color: Theme.line
            Rectangle {
                width: parent.width * (screen.job.frames ? screen.job.frame / screen.job.frames : 0)
                height: parent.height
                radius: 4
                color: screen.job.error ? Theme.err : screen.job.active ? Theme.accent : Theme.ok
            }
        }
        Label {
            Layout.fillWidth: true
            text: {
                const j = screen.job, fps = j.fps || 30
                return qsTr("frame %1 / %2 · %3 %").arg(j.frame || 0).arg(j.frames || 0)
                           .arg(j.frames ? (100 * j.frame / j.frames).toFixed(1) : "0.0")
                       + " · " + qsTr("out %1 / %2").arg(screen.clock((j.frame || 0) * 1000 / fps))
                                                     .arg(screen.clock((j.frames || 0) * 1000 / fps))
                       + " · " + qsTr("elapsed %1").arg(screen.clock(j.elapsedMs || 0))
                       + " · " + qsTr("%1 frames/s").arg((j.rate || 0).toFixed(2))
                       + (j.active ? " · " + qsTr("ETA %1").arg(screen.clock(j.etaMs || 0)) : "")
                       + ((j.videoGaps || 0) > 0 ? " · " + qsTr("%1 video gaps").arg(j.videoGaps) : "")
            }
            color: (screen.job.videoGaps || 0) > 0 ? Theme.warn : Theme.fg
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
            wrapMode: Text.Wrap
        }

        Label {
            Layout.topMargin: 10
            text: qsTr("PER FRAME · MS · LAST 20 FRAMES · %1 MS").arg(screen.total.toFixed(0))
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        Row {
            Layout.fillWidth: true
            height: 12
            clip: true
            Repeater {
                model: screen.rows
                Rectangle {
                    required property var modelData
                    width: screen.total > 0 ? parent.width * (screen.stages[modelData[0]] || 0) / screen.total : 0
                    height: parent.height
                    color: modelData[2]
                }
            }
        }
        Repeater {
            model: screen.rows
            RowLayout {
                required property var modelData
                spacing: 8
                Rectangle { implicitWidth: 8; implicitHeight: 8; radius: 2; color: parent.modelData[2] }
                Label {
                    Layout.preferredWidth: 140
                    text: parent.modelData[1]
                    color: Theme.fg
                    font.pixelSize: Theme.small + 1
                }
                Label {
                    Layout.preferredWidth: 60
                    horizontalAlignment: Text.AlignRight
                    text: (screen.stages[parent.modelData[0]] || 0).toFixed(1)
                    color: Theme.fg
                    font.family: Theme.mono
                    font.pixelSize: Theme.small + 1
                }
                Label {
                    text: screen.total > 0 ? (100 * (screen.stages[parent.modelData[0]] || 0) / screen.total).toFixed(0) + " %" : ""
                    color: Theme.muted
                    font.family: Theme.mono
                    font.pixelSize: Theme.small + 1
                }
            }
        }
        Label {
            text: qsTr("encoder queue %1 / 4").arg(screen.job.queue || 0)
            color: (screen.job.queue || 0) >= 4 ? Theme.warn : Theme.muted
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
        }

        Label {
            Layout.topMargin: 10
            text: qsTr("DEVICE · ENCODER")
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        Label {
            Layout.fillWidth: true
            text: (screen.job.backend || "?") + " · " + (screen.job.device || "?") + "\n"
                  + (screen.job.encoder ? screen.job.encoder : qsTr("encoder: unknown"))
            color: Theme.fg
            font.family: Theme.mono
            font.pixelSize: Theme.small + 1
            wrapMode: Text.Wrap
        }

        Label {
            Layout.topMargin: 10
            visible: (screen.job.previewSerial || 0) > 0
            text: qsTr("LAST FRAME")
            color: Theme.muted
            font.pixelSize: 8
            font.letterSpacing: 1.2
        }
        Image {
            visible: (screen.job.previewSerial || 0) > 0
            Layout.preferredWidth: 480
            Layout.preferredHeight: 480 * (screen.job.height || 9) / (screen.job.width || 16)
            source: (screen.job.previewSerial || 0) > 0 ? "image://render/" + screen.job.previewSerial : ""
            cache: false
            asynchronous: true
            fillMode: Image.PreserveAspectFit
        }
        Label {
            visible: !!screen.job.error
            Layout.fillWidth: true
            text: screen.job.error || ""
            color: Theme.err
            wrapMode: Text.Wrap
            font.pixelSize: Theme.small + 1
        }
    }
}
