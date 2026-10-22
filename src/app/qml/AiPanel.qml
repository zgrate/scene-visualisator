import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Project screen's second card: AI access (the MCP server an AI client connects to, docs/mcp-plan.md) and debug mode
// with the log tail.
Rectangle {
    id: panel
    readonly property var ai: Session.aiStatus
    property bool showToken: false

    width: 500
    height: col.implicitHeight + 32
    color: Theme.panel
    border.color: Theme.line
    radius: Theme.radius

    component Section: Label {
        color: Theme.muted
        font.pixelSize: Theme.small
        font.letterSpacing: 1
        topPadding: 6
    }
    component Mono: Label {
        color: Theme.fg
        font.family: Theme.mono
        font.pixelSize: Theme.small + 1
        elide: Text.ElideMiddle
        Layout.fillWidth: true
    }

    ColumnLayout {
        id: col
        anchors { fill: parent; margins: 16 }
        spacing: 8
        Section { text: qsTr("AI ACCESS (MCP)"); topPadding: 0 }
        Check {
            text: qsTr("Enable AI access")
            checked: panel.ai.enabled
            onToggled: Session.setAiAccess("enabled", checked)
        }
        Check {
            text: qsTr("Allow LAN (plain HTTP: show network or VPN only)")
            checked: panel.ai.lan
            onToggled: Session.setAiAccess("lan", checked)
        }
        Check {
            text: qsTr("Allow file access (open, import, export by path)")
            checked: panel.ai.files
            onToggled: Session.setAiAccess("files", checked)
        }
        RowLayout {
            spacing: 6
            Label { text: qsTr("Port"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            Input {
                implicitWidth: 64
                text: panel.ai.port
                validator: IntValidator { bottom: 1024; top: 65535 }
                onEditingFinished: if (Number(text) !== panel.ai.port) Session.setAiAccess("port", Number(text))
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.pixelSize: Theme.small + 1
                color: panel.ai.error ? Theme.err : panel.ai.listening ? Theme.ok : Theme.muted
                text: panel.ai.error ? panel.ai.error
                    : !panel.ai.listening ? qsTr("Off")
                    : qsTr("Listening on %1 · %2 client(s) · %3 call(s)%4")
                          .arg(panel.ai.addresses.map(a => a + ":" + panel.ai.port).join(", "))
                          .arg(panel.ai.clients).arg(panel.ai.calls)
                          .arg(panel.ai.lastTool ? " · " + panel.ai.lastTool : "")
            }
        }
        RowLayout {
            spacing: 6
            Label { text: qsTr("Token"); color: Theme.muted; font.pixelSize: Theme.small + 1 }
            Mono { text: panel.showToken ? panel.ai.token : "•".repeat(12) }
            Button { text: panel.showToken ? qsTr("Hide") : qsTr("Show"); onClicked: panel.showToken = !panel.showToken }
            Button { text: qsTr("Copy"); onClicked: Session.copyText(panel.ai.token) }
            Button {
                text: qsTr("Regenerate")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Connected AI clients stop working until they get the new token")
                onClicked: Session.regenerateAiToken()
            }
        }
        Label {
            text: qsTr("Connect Claude Code (run on the AI's machine):")
            color: Theme.muted
            font.pixelSize: Theme.small + 1
        }
        RowLayout {
            spacing: 6
            Mono { text: panel.ai.command.replace(panel.ai.token, panel.showToken ? panel.ai.token : "…") }
            Button { text: qsTr("Copy"); onClicked: Session.copyText(panel.ai.command) }
        }

        Section { text: qsTr("DEBUG") }
        Check {
            text: qsTr("Debug mode (HUD, debug log, debug AI tools)")
            checked: Session.debugMode
            onToggled: Session.setDebugMode(checked)
        }
        ScrollView {
            visible: Session.debugMode
            Layout.fillWidth: true
            Layout.preferredHeight: 220
            TextArea {
                id: log
                readOnly: true
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.fg
                font.family: Theme.mono
                font.pixelSize: Theme.small
                background: Rectangle { color: Theme.viewport; radius: 3; border.color: Theme.line }
            }
        }
        RowLayout {
            visible: Session.debugMode
            spacing: 6
            Input { id: filter; Layout.fillWidth: true; placeholderText: qsTr("Filter, e.g. sv.mcp or warning") }
            Button { text: qsTr("Copy log"); onClicked: Session.copyText(Session.logText(2000, filter.text)) }
        }
    }
    Timer { // a text property bound to the log would rebuild on every message; once a second is plenty
        running: Session.debugMode && panel.visible
        repeat: true
        triggeredOnStart: true
        interval: 1000
        onTriggered: {
            log.text = Session.logText(500, filter.text)
            log.cursorPosition = log.length
        }
    }
}
