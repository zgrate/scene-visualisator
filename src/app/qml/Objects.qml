import QtQuick
import QtQml.Models
import QtQuick.Controls
import QtQuick.Layouts

// Scene object list: click selects, Ctrl/Cmd-click toggles. The + menu adds fixtures, walls and trusses.
Column {
    id: objs
    width: parent ? parent.width : 0
    spacing: 6

    ListView {
        id: list
        width: parent.width
        height: Math.min(contentHeight, 320)
        clip: true
        model: Session.objects
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }
        delegate: Rectangle {
            required property var modelData
            width: ListView.view.width
            height: 22
            radius: 3
            color: modelData.selected ? Theme.accentSoft : row.hovered ? Theme.line : "transparent"
            Rectangle {
                visible: modelData.selected
                width: 2; height: parent.height
                color: Theme.accent
            }
            RowLayout {
                anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                Label {
                    text: modelData.name
                    color: Theme.fg
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSize
                    Layout.fillWidth: true
                }
                Label {
                    text: modelData.detail
                    color: Theme.muted
                    font.family: Theme.mono
                    font.pixelSize: Theme.small + 1
                }
            }
            HoverHandler { id: row }
            TapHandler {
                onTapped: (point, button) => Session.selectUuid(modelData.uuid,
                                                                 point.modifiers & (Qt.ControlModifier | Qt.MetaModifier))
            }
        }
    }

    Button {
        text: qsTr("+ Add")
        font.pixelSize: Theme.small + 1
        onClicked: addMenu.popup()
        Menu {
            id: addMenu
            Menu {
                id: fixtureMenu
                title: qsTr("Fixture")
                Instantiator {
                    model: Session.fixtureTypes
                    MenuItem {
                        required property string modelData
                        text: modelData.replace(/\.gdtf$/, "").replace("@", " ").replace(/_/g, " ")
                        onTriggered: Session.addFixture(modelData, Session.modes(modelData)[0])
                    }
                    onObjectAdded: (i, item) => fixtureMenu.insertItem(i, item)
                    onObjectRemoved: (i, item) => fixtureMenu.removeItem(item)
                }
            }
            MenuItem { text: qsTr("LED wall"); onTriggered: Session.addWall() }
            MenuItem { text: qsTr("Truss"); onTriggered: Session.addTruss() }
        }
    }
}
