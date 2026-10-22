pragma Singleton
import QtQuick

// Studio dark tokens (AGENTS.md "Design rules"). Components take every colour and size from here.
QtObject {
    readonly property color bg: "#121418"
    readonly property color panel: "#1b1e24"
    readonly property color line: "#2c3039"
    readonly property color fg: "#e8e6e1"
    readonly property color muted: "#a09d95"
    readonly property color viewport: "#0b0c10"
    readonly property color accent: "#4c8dff"
    readonly property color accentOn: "#ffffff"
    readonly property color accentSoft: "#1d2f4d"
    readonly property color ok: "#3ddc97"
    readonly property color warn: "#ffd166"
    readonly property color err: "#ff6b6b"
    readonly property color field: "#14161b"
    readonly property color control: "#23262e"
    // DMX monitor winner stripes, by source index; kept apart from the ok/warn/err trio.
    readonly property var sources: ["#4c8dff", "#c792ea", "#4dd0e1", "#f78c6c"]

    readonly property int rail: 36
    readonly property int titleBar: 30
    readonly property int dock: 46
    readonly property int radius: 8
    readonly property int gap: 10

    readonly property string mono: Qt.platform.os === "windows" ? "Consolas"
                                 : Qt.platform.os === "osx" ? "Menlo" : "DejaVu Sans Mono"
    readonly property int fontSize: 12
    readonly property int small: 10
}
