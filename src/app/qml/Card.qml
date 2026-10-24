import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// Floating card over the viewport: drag by the header, click the header to collapse. Position and collapse
// state persist in the project layout under layout.cards.<key>.
Item {
    id: card
    required property string key
    property string title
    property bool collapsed: false
    default property alias content: body.data
    property real defaultX: 0
    property real defaultY: 0

    width: 260
    height: header.height + (collapsed ? 0 : body.childrenRect.height + 16)

    property bool placed: false
    function restore() {
        if (!parent || parent.width <= 0) return
        placed = true
        const s = (Session.layout.cards || {})[key]
        x = s ? Math.max(0, Math.min(s.x, parent.width - width)) : defaultX
        y = s ? Math.max(0, Math.min(s.y, parent.height - header.height)) : defaultY
        collapsed = s ? !!s.collapsed : false
    }
    function save() {
        const cards = Object.assign({}, Session.layout.cards || {})
        cards[key] = { x: Math.round(x), y: Math.round(y), collapsed: collapsed }
        Session.setLayout("cards", cards)
    }
    Component.onCompleted: restore()
    Connections {
        target: card.parent
        function onWidthChanged() { if (!card.placed) card.restore() }
    }

    RectangularShadow {
        anchors.fill: bg
        radius: bg.radius
        offset.y: 4
        blur: 12
        color: "#73000000"
    }
    Rectangle {
        id: bg
        anchors.fill: parent
        color: Theme.panel
        border.color: Theme.line
        radius: Theme.radius
        // Clicks and wheel on the card's empty parts stop here instead of selecting or zooming in the viewport below
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
    }
    Item {
        id: header
        width: parent.width
        height: 28
        Label {
            anchors.verticalCenter: parent.verticalCenter
            x: 10
            text: card.title.toUpperCase()
            font.pixelSize: Theme.small
            font.letterSpacing: 1
            color: Theme.muted
        }
        Label {
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: card.collapsed ? "+" : "–"
            color: Theme.muted
        }
        DragHandler {
            target: card
            xAxis { minimum: 0; maximum: card.parent.width - card.width }
            yAxis { minimum: 0; maximum: card.parent.height - header.height }
            onActiveChanged: if (!active) card.save()
        }
        TapHandler {
            onTapped: { card.collapsed = !card.collapsed; card.save() }
        }
    }
    Item {
        id: body
        visible: !card.collapsed
        x: 8
        y: header.height
        width: parent.width - 16
        height: childrenRect.height
    }
}
