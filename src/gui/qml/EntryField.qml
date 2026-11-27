import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root
    required property UiTheme theme
    property bool invalid: false
    property string toolTip: ""
    hoverEnabled: true
    ToolTip.visible: hovered && toolTip !== ""
    ToolTip.text: toolTip
    ToolTip.delay: 600
    Accessible.description: toolTip
    implicitHeight: 34
    font.pixelSize: 13
    leftPadding: 10
    rightPadding: 10
    color: theme.text
    placeholderTextColor: theme.muted
    selectionColor: theme.accent
    selectedTextColor: "white"
    selectByMouse: true
    background: Rectangle {
        radius: 6
        color: root.theme.subtle
        border.width: root.activeFocus ? 2 : 1
        border.color: root.invalid ? root.theme.error : root.activeFocus ? root.theme.accent : root.theme.line
    }
}
