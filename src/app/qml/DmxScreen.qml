import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SceneVisualiser

// DMX monitor: universes on the left (patched ones plus any that receive data), the 512-channel grid on the right
// with sources, merged rate and packet loss. The selected fixture's channels are outlined in the grid.
Rectangle {
    id: mon
    color: Theme.bg
    readonly property var per: Session.dmxStatus.perUniverse || ({})
    readonly property var list: {
        const u = new Set(Session.universes)
        Object.keys(per).forEach(k => u.add(Number(k)))
        return Array.from(u).sort((a, b) => a - b)
    }
    property int universe: Session.current.patch ? Session.current.patch.universe : (list.length ? list[0] : 1)
    readonly property var st: per[universe] || { rate: 0, sources: [], lost: 0, dropped: 0 }
    readonly property var sourceColors: Theme.sources

    RowLayout {
        anchors { fill: parent; margins: 16 }
        spacing: 12

        Rectangle {
            Layout.preferredWidth: 170
            Layout.fillHeight: true
            color: Theme.panel
            border.color: Theme.line
            radius: Theme.radius
            ColumnLayout {
                anchors { fill: parent; margins: 8 }
                spacing: 4
                Label { text: qsTr("UNIVERSES"); color: Theme.muted; font.pixelSize: Theme.small; font.letterSpacing: 1 }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: mon.list
                    delegate: Rectangle {
                        required property int modelData
                        readonly property var s: mon.per[modelData]
                        width: ListView.view.width
                        height: 24
                        radius: 3
                        color: mon.universe === modelData ? Theme.accentSoft : hover.hovered ? Theme.line : "transparent"
                        HoverHandler { id: hover }
                        TapHandler { onTapped: mon.universe = modelData }
                        RowLayout {
                            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                            Label {
                                textFormat: Text.StyledText
                                text: "<font color='" + (s && s.sources.length ? Theme.ok : Theme.line) + "'>●</font> "
                                      + qsTr("Universe %1").arg(modelData)
                                color: Theme.fg
                                font.pixelSize: Theme.fontSize
                                Layout.fillWidth: true
                            }
                            Label {
                                text: s ? Math.round(s.rate) : "—"
                                color: Theme.muted
                                font.family: Theme.mono
                                font.pixelSize: Theme.small + 1
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8
            RowLayout {
                spacing: 16
                Label { text: qsTr("Universe %1").arg(mon.universe); color: Theme.fg; font.bold: true; font.pixelSize: 14 }
                Repeater {
                    model: [[qsTr("rate"), mon.st.sources.length ? Math.round(mon.st.rate) + " Hz" : "—"],
                            [qsTr("lost"), mon.st.lost], [qsTr("dropped"), mon.st.dropped]]
                    Label {
                        required property var modelData
                        textFormat: Text.StyledText
                        text: modelData[0] + " <font color='" + Theme.fg + "'>" + modelData[1] + "</font>"
                        color: Theme.muted
                        font.family: Theme.mono
                        font.pixelSize: Theme.small + 1
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: !mon.st.sources.length
                    text: qsTr("no source")
                    color: Theme.muted
                    font.pixelSize: Theme.small + 1
                }
                Repeater {
                    model: mon.st.sources
                    Label {
                        required property string modelData
                        required property int index
                        textFormat: Text.StyledText
                        text: "<font color='" + mon.sourceColors[index % mon.sourceColors.length] + "'>▬</font> " + modelData
                        color: Theme.fg
                        font.pixelSize: Theme.small + 1
                    }
                }
            }
            ChannelGrid {
                Layout.fillWidth: true
                Layout.fillHeight: true
                universe: mon.universe
                font.family: Theme.mono
                font.pixelSize: Theme.small + 1
                textColor: Theme.fg
                mutedColor: Theme.muted
                lineColor: Theme.line
                levelColor: Theme.accent
                highlightColor: Theme.warn
                conflictColor: Theme.err
                sourceColors: mon.sourceColors
            }
        }
    }
}
