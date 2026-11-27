pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    required property UiTheme theme
    property list<string> options
    property int currentIndex: 0
    property bool vertical: false
    implicitWidth: optionsRow.implicitWidth + 6
    implicitHeight: vertical ? optionsRow.implicitHeight + 6 : 34
    radius: 8
    color: theme.subtle
    border.color: theme.line
    GridLayout {
        id: optionsRow
        anchors.fill: parent
        anchors.margins: 3
        columns: root.vertical ? 1 : Math.max(1, root.options.length)
        rowSpacing: 3
        columnSpacing: 3
        Repeater {
            model: root.options
            delegate: ActionButton {
                id: optionButton
                required property int index
                required property string modelData
                theme: root.theme
                quiet: root.currentIndex !== index
                text: modelData
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitWidth: Math.max(72, contentItem.implicitWidth + 24)
                font.pixelSize: 12
                font.weight: root.currentIndex === index ? Font.DemiBold : Font.Normal
                onClicked: root.currentIndex = index
                background: Rectangle {
                    radius: 5
                    color: root.currentIndex === optionButton.index ? root.theme.surface
                                                                   : optionButton.hovered ? root.theme.hover : root.theme.subtle
                    border.color: optionButton.visualFocus ? root.theme.accent : "transparent"
                }
            }
        }
    }
}
