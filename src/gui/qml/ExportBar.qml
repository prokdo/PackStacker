pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: bar
    required property UiTheme theme
    required property bool locked
    required property bool failed
    required property bool completed
    required property bool cancelling
    required property bool ready
    required property string message
    required property string details
    required property string resultPath
    signal startRequested()
    signal cancelRequested()
    signal resultRequested()
    signal detailsRequested()

    implicitHeight: 68
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 9
        Rectangle {
            Layout.preferredWidth: 7
            Layout.preferredHeight: 7
            radius: 4
            objectName: "statusIndicator"
            color: bar.failed ? bar.theme.error : bar.locked ? bar.theme.accent : bar.completed ? bar.theme.success : bar.theme.muted
        }
        Label {
            objectName: "statusText"
            Layout.fillWidth: true
            text: bar.message
            color: bar.failed ? bar.theme.error : bar.theme.text
            font.pixelSize: 12
            elide: Text.ElideRight
            textFormat: Text.PlainText
            ToolTip.visible: statusHover.hovered
            ToolTip.text: text
            HoverHandler { id: statusHover }
        }
        ActionButton {
            objectName: "showResult"
            theme: bar.theme
            quiet: true
            text: qsTranslate("Main", "Показать результат в папке")
            visible: !bar.locked && bar.resultPath !== ""
            toolTip: bar.resultPath
            onClicked: bar.resultRequested()
        }
        ActionButton {
            theme: bar.theme
            quiet: true
            text: qsTranslate("Main", "Подробности")
            font.pixelSize: 11
            visible: bar.details !== ""
            onClicked: bar.detailsRequested()
        }
        ActionButton {
            objectName: "cancelExport"
            theme: bar.theme
            text: qsTranslate("Main", "Отменить")
            visible: bar.locked
            enabled: bar.locked && !bar.cancelling
            onClicked: bar.cancelRequested()
        }
        ActionButton {
            objectName: "startExport"
            theme: bar.theme
            text: qsTranslate("Main", "Объединить")
            glyph: "layers"
            primary: true
            Layout.preferredWidth: 172
            visible: !bar.locked
            enabled: !bar.locked && bar.ready
            onClicked: bar.startRequested()
        }
    }
    ProgressBar {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 2
        visible: bar.locked
        indeterminate: true
    }
}
