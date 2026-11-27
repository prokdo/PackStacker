import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Button {
    id: root
    required property UiTheme theme
    property string glyph: ""
    property bool quiet: false
    property bool primary: false
    property string toolTip: text === "" ? Accessible.name : ""
    ToolTip.visible: hovered && toolTip !== ""
    ToolTip.text: toolTip
    ToolTip.delay: 600
    implicitHeight: primary ? 38 : 34
    implicitWidth: text === "" ? 34 : Math.max(34, contents.implicitWidth + 24)
    padding: 8
    hoverEnabled: true
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.4
    contentItem: RowLayout {
        id: contents
        spacing: 8
        Glyph {
            visible: root.glyph !== ""
            name: root.glyph
            color: root.primary ? "white" : root.quiet && !root.hovered ? root.theme.muted : root.theme.text
            Layout.alignment: Qt.AlignVCenter
        }
        Text {
            visible: root.text !== ""
            text: root.text
            font: root.font
            color: root.primary ? "white" : root.quiet && !root.hovered ? root.theme.muted : root.theme.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            Layout.fillWidth: true
        }
    }
    background: Rectangle {
        radius: 7
        color: root.primary ? (root.hovered ? "#1d4ed8" : "#2563eb")
                             : root.down || root.hovered ? root.theme.hover
                                                        : root.theme.subtle
        border.width: 1
        border.color: root.visualFocus ? root.theme.accent : root.primary ? "#2563eb" : root.theme.line
    }
}
