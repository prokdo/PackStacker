import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ItemDelegate {
    id: root
    required property UiTheme theme
    required property var entry
    property int rank: 0
    property bool dragEnabled: false
    property bool dropTarget: false
    signal dragStarted(point scene)
    signal dragMoved(point scene)
    signal dragFinished(point scene)
    signal dragCancelled()
    property string typeWarning: ""
    readonly property string warning: typeWarning || entry.warning || ""
    implicitHeight: warning && !entry.error ? 98 : 78
    leftPadding: 10
    rightPadding: 10
    hoverEnabled: true
    Accessible.name: entry.name + ". " + (entry.error || entry.description) + (warning ? ". " + warning : "")
    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: entry.path + (entry.error ? "\n" + entry.error : "")
                  + (typeWarning ? "\n" + typeWarning : "") + (entry.warning ? "\n" + entry.warning : "")
    DragHandler {
        id: rowDrag
        enabled: root.dragEnabled
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        cursorShape: Qt.ClosedHandCursor
        grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.ApprovesCancellation
        onActiveTranslationChanged: {
            if (active)
                root.dragMoved(centroid.scenePosition)
        }
        onGrabChanged: (transition, point) => {
            if (transition === PointerDevice.GrabExclusive)
                root.dragStarted(point.scenePosition)
            else if (transition === PointerDevice.UngrabExclusive && point.state === EventPoint.Released)
                root.dragFinished(point.scenePosition)
            else if (transition === PointerDevice.CancelGrabExclusive || transition === PointerDevice.UngrabExclusive)
                root.dragCancelled()
        }
    }
    background: Rectangle {
        radius: 7
        color: root.highlighted ? root.theme.selection : root.hovered ? root.theme.hover : "transparent"
        border.width: 1
        border.color: root.dropTarget ? root.theme.accent
                    : root.highlighted || root.visualFocus ? root.theme.selectionBorder : "transparent"
    }
    contentItem: RowLayout {
        spacing: 12
        Text {
            visible: root.rank > 0
            text: root.rank
            color: root.highlighted ? root.theme.accent : root.theme.muted
            font.pixelSize: 11
            Layout.preferredWidth: 13
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 4
        }
        Rectangle {
            Layout.preferredWidth: 46
            Layout.preferredHeight: 46
            radius: 7
            color: root.theme.selection
            Image {
                id: packImage
                objectName: "packIcon"
                anchors.fill: parent
                source: root.entry.icon
                fillMode: Image.PreserveAspectFit
                sourceSize: Qt.size(96, 96)
                visible: status === Image.Ready
            }
            Glyph {
                anchors.centerIn: parent
                width: 25
                height: 25
                visible: packImage.status !== Image.Ready
                color: root.theme.accent
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                Text {
                    text: root.entry.name
                    color: root.theme.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    Layout.maximumWidth: Math.max(30, root.width - 195)
                }
                Rectangle {
                    implicitWidth: storage.implicitWidth + 10
                    implicitHeight: 16
                    radius: 4
                    color: root.theme.surface
                    border.color: root.theme.line
                    Text {
                        id: storage
                        anchors.centerIn: parent
                        text: root.entry.storage
                        color: root.theme.muted
                        font.pixelSize: 9
                    }
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                Layout.fillWidth: true
                text: root.entry.error || root.entry.description || qsTr("Без описания")
                color: root.entry.error ? root.theme.error : root.theme.muted
                font.pixelSize: 12
                textFormat: Text.PlainText
                elide: Text.ElideRight
                maximumLineCount: 1
            }
            Text {
                Layout.fillWidth: true
                text: root.entry.error ? qsTr("Не удалось прочитать пак")
                      : root.entry.format ? qsTr("Формат %1 · %2").arg(root.entry.format).arg(root.entry.size)
                                        : qsTr("Будет проверен при объединении")
                color: root.theme.muted
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            RowLayout {
                visible: !!root.warning && !root.entry.error
                Layout.fillWidth: true
                spacing: 5
                Glyph { name: "warning"; color: root.theme.warning; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                Text {
                    objectName: "packWarning"
                    Layout.fillWidth: true
                    text: qsTr("Предупреждение: %1").arg(root.warning)
                    textFormat: Text.PlainText
                    color: root.theme.warning
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }
}
