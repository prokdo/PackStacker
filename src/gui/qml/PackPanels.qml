pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import PackStacker

RowLayout {
    id: packPanels
    required property PackController controller
    required property UiTheme theme
    required property bool locked
    required property int selectedType
    required property string targetFormat
    required property int catalogRevision
    property string selectedPackPath: ""
    property string draggedPath: ""
    property string draggedName: ""
    property bool draggedFromStack: false
    property point pointer
    readonly property bool dragging: draggedPath !== "" && !locked
    readonly property bool overStack: dragging && over(stack, pointer)
    readonly property bool overAvailable: dragging && draggedFromStack && over(available, pointer)
    readonly property int dropIndex: overStack ? stackIndexAt(pointer) : -1

    readonly property real headerHeight: Math.max(availableHeader.implicitHeight, stackHeader.implicitHeight)
    readonly property real headerSpacing: 10
    spacing: 0

    function typeWarning(path: string): string {
        const revision = catalogRevision
        return controller.PackTypeWarning(path, selectedType, targetFormat)
    }

    function addAvailablePack(index: int, destination: int) {
        const first = stack.count
        if (controller.AddAvailable(index)) {
            if (destination >= 0 && destination < first)
                controller.MovePack(first, destination)
            stack.forceLayout()
            selectPack(destination >= 0 ? Math.min(destination, first) : first)
        }
    }

    function returnPack(index: int) {
        const path = controller.packs[index]
        if (controller.RemovePack(index)) {
            selectPack(Math.min(index, controller.packs.length - 1))
            Qt.callLater(function() {
                const entries = packPanels.controller.availablePacks
                for (let i = 0; i < entries.length; ++i) {
                    if (entries[i].path === path) {
                        available.currentIndex = i
                        available.positionViewAtIndex(i, ListView.Contain)
                        break
                    }
                }
            })
        }
    }

    function over(view: ListView, point: point): bool {
        const local = view.mapFromItem(packPanels, point)
        return local.x >= 0 && local.x < view.width && local.y >= 0 && local.y < view.height
    }

    function stackIndexAt(point: point): int {
        const local = stack.contentItem.mapFromItem(packPanels, point)
        const index = stack.indexAt(local.x, local.y)
        if (index >= 0)
            return index
        const next = stack.indexAt(local.x, local.y + stack.spacing)
        return next >= 0 ? next : stack.count
    }

    function beginDrag(entry: var, fromStack: bool, scene: point) {
        draggedPath = entry.path
        draggedName = entry.name
        draggedFromStack = fromStack
        updateDrag(scene)
    }

    function updateDrag(scene: point) {
        pointer = mapFromItem(null, scene)
    }

    function finishDrag(scene: point) {
        updateDrag(scene)
        const path = draggedPath
        const fromStack = draggedFromStack
        const toStack = overStack
        const toAvailable = overAvailable
        const destination = toStack ? stackIndexAt(pointer) : -1
        draggedPath = ""
        if (!toStack && !toAvailable)
            return
        Qt.callLater(function() {
            if (packPanels.locked)
                return
            if (fromStack) {
                const index = packPanels.controller.packs.indexOf(path)
                if (index < 0)
                    return
                if (toAvailable) {
                    packPanels.returnPack(index)
                } else {
                    const to = Math.min(destination, stack.count - 1)
                    if (packPanels.controller.MovePack(index, to))
                        packPanels.selectPack(to)
                }
            } else {
                const entries = packPanels.controller.availablePacks
                for (let index = 0; index < entries.length; ++index) {
                    if (entries[index].path === path) {
                        packPanels.addAvailablePack(index, destination)
                        break
                    }
                }
            }
        })
    }

    function selectPack(index: int) {
        stack.currentIndex = index
        selectedPackPath = index >= 0 && index < controller.packs.length ? controller.packs[index] : ""
        if (index >= 0) {
            stack.forceLayout()
            stack.positionViewAtIndex(index, ListView.Contain)
        }
    }

    function restoreSelection() {
        let index = -1
        for (let i = 0; i < controller.packs.length; ++i) {
            if (controller.packs[i] === selectedPackPath)
                index = i
        }
        if (index < 0 && controller.packs.length > 0)
            index = 0
        stack.forceLayout()
        selectPack(index)
    }

    Dialogs.FolderDialog {
        id: sourceFolder
        objectName: "sourceFolder"
        title: qsTranslate("Main", "Выбрать каталог с доступными паками")
        onAccepted: packPanels.controller.SetSourceDirectory(selectedFolder)
    }

    Rectangle {
        objectName: "availablePanel"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 1
        radius: 10
        color: packPanels.theme.surface
        border.color: packPanels.overAvailable ? packPanels.theme.accent : packPanels.theme.line
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 15
            spacing: packPanels.headerSpacing
            RowLayout {
                id: availableHeader
                objectName: "availableHeader"
                Layout.fillWidth: true
                Layout.minimumHeight: packPanels.headerHeight
                Layout.maximumHeight: packPanels.headerHeight
                spacing: 7
                Label { text: qsTranslate("Main", "Доступные паки"); font.weight: Font.DemiBold; font.pixelSize: 14 }
                Label { text: available.count; color: packPanels.theme.muted; font.pixelSize: 11 }
                EntryField {
                    objectName: "sourcePath"
                    theme: packPanels.theme
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: packPanels.controller.sourceDirectory
                    readOnly: true
                    placeholderText: qsTranslate("Main", "Выберите исходный каталог…")
                    Accessible.name: qsTranslate("Main", "Исходный каталог")
                }
                ActionButton {
                    objectName: "browseSource"
                    theme: packPanels.theme
                    glyph: "open"
                    quiet: true
                    enabled: !packPanels.locked
                    Accessible.name: qsTranslate("Main", "Выбрать каталог паков")
                    onClicked: sourceFolder.open()
                }
                ActionButton {
                    objectName: "refreshCatalog"
                    theme: packPanels.theme
                    glyph: "refresh"
                    quiet: true
                    enabled: !packPanels.locked && (packPanels.controller.sourceDirectory !== ""
                             || packPanels.controller.packs.length > 0)
                    toolTip: qsTr("Обновить каталог")
                    Accessible.name: toolTip
                    onClicked: packPanels.controller.RefreshCatalog()
                }
            }
            ListView {
                id: available
                objectName: "availablePacks"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3
                model: packPanels.controller.availablePacks
                enabled: !packPanels.locked
                ScrollBar.vertical: ScrollBar {}
                delegate: PackRow {
                    required property var modelData
                    required property int index
                    objectName: "availablePack" + index
                    width: ListView.view.width
                    theme: packPanels.theme
                    entry: modelData
                    typeWarning: packPanels.typeWarning(entry.path)
                    highlighted: ListView.isCurrentItem
                    onClicked: available.currentIndex = index
                    onDoubleClicked: packPanels.addAvailablePack(index, -1)
                    dragEnabled: !entry.error
                    onDragStarted: scene => packPanels.beginDrag(entry, false, scene)
                    onDragMoved: scene => packPanels.updateDrag(scene)
                    onDragFinished: scene => packPanels.finishDrag(scene)
                    onDragCancelled: packPanels.draggedPath = ""
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - 20
                    visible: available.count === 0
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: packPanels.controller.scanning ? qsTranslate("Main", "Чтение каталога…")
                              : packPanels.controller.sourceDirectory === "" ? qsTranslate("Main", "Выберите папку с паками")
                                                                       : qsTranslate("Main", "Нет доступных паков")
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTranslate("Main", "Папки паков и ZIP из выбранного каталога появятся здесь.")
                        wrapMode: Text.WordWrap
                        color: packPanels.theme.muted
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    ColumnLayout {
        Layout.preferredWidth: 54
        Layout.alignment: Qt.AlignVCenter
        Layout.topMargin: packPanels.headerHeight + packPanels.headerSpacing
        spacing: 9
        ActionButton {
            objectName: "addAvailable"
            theme: packPanels.theme
            glyph: "right"
            Layout.alignment: Qt.AlignHCenter
            enabled: !packPanels.locked && available.currentIndex >= 0 && available.count > 0
            Accessible.name: qsTranslate("Main", "Добавить выбранный пак в стек")
            onClicked: packPanels.addAvailablePack(available.currentIndex, -1)
        }
        ActionButton {
            objectName: "removePack"
            theme: packPanels.theme
            glyph: "left"
            Layout.alignment: Qt.AlignHCenter
            enabled: !packPanels.locked && stack.currentIndex >= 0 && stack.count > 0
            Accessible.name: qsTranslate("Main", "Вернуть выбранный пак в доступные")
            toolTip: qsTranslate("Main", "Вернуть в левый список. Исходные файлы не изменяются.")
            onClicked: packPanels.returnPack(stack.currentIndex)
        }
    }

    Rectangle {
        objectName: "selectedPanel"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 1
        radius: 10
        color: packPanels.theme.surface
        border.color: packPanels.overStack ? packPanels.theme.accent : packPanels.theme.line
        Rectangle {
            parent: Overlay.overlay
            readonly property point anchorPoint: packPanels.mapToItem(parent, packPanels.pointer)
            visible: packPanels.dragging
            z: 10
            x: anchorPoint.x + 12
            y: anchorPoint.y + 12
            width: Math.min(260, dragLabel.implicitWidth + 20)
            height: dragLabel.implicitHeight + 10
            radius: 6
            color: packPanels.theme.surface
            border.color: packPanels.theme.accent
            Label {
                id: dragLabel
                anchors.fill: parent
                anchors.margins: 5
                text: packPanels.draggedName
                textFormat: Text.PlainText
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                color: packPanels.theme.text
            }
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 15
            spacing: packPanels.headerSpacing
            RowLayout {
                id: stackHeader
                Layout.fillWidth: true
                Layout.minimumHeight: packPanels.headerHeight
                Layout.maximumHeight: packPanels.headerHeight
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 2
                    RowLayout {
                        Label { objectName: "stackTitle"; text: qsTranslate("Main", "Выбранный стек"); font.weight: Font.DemiBold; font.pixelSize: 14 }
                        Label { text: stack.count; color: packPanels.theme.muted; font.pixelSize: 11 }
                    }
                    Label {
                        objectName: "priorityHint"
                        Layout.fillWidth: true
                        text: qsTranslate("Main", "Выше в списке — выше приоритет")
                        color: packPanels.theme.muted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
                ActionButton {
                    objectName: "clearPacks"
                    theme: packPanels.theme
                    quiet: true
                    text: qsTranslate("Main", "Очистить стек")
                    font.pixelSize: 11
                    enabled: !packPanels.locked && stack.count > 0
                    onClicked: packPanels.controller.ClearPacks()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10
                ListView {
                    id: stack
                    objectName: "packStack"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 3
                    enabled: !packPanels.locked
                    model: packPanels.controller.selectedPacks
                    ScrollBar.vertical: ScrollBar {}
                    delegate: PackRow {
                        required property var modelData
                        required property int index
                        objectName: "stackPack" + index
                        width: ListView.view.width
                        theme: packPanels.theme
                        entry: modelData
                        typeWarning: packPanels.typeWarning(entry.path)
                        rank: index + 1
                        highlighted: ListView.isCurrentItem
                        dropTarget: index === packPanels.dropIndex
                        onClicked: packPanels.selectPack(index)
                        dragEnabled: true
                        onDragStarted: scene => packPanels.beginDrag(entry, true, scene)
                        onDragMoved: scene => packPanels.updateDrag(scene)
                        onDragFinished: scene => packPanels.finishDrag(scene)
                        onDragCancelled: packPanels.draggedPath = ""
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 20
                        visible: stack.count === 0
                        spacing: 4
                        Glyph { Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 34; Layout.preferredHeight: 34; color: packPanels.theme.muted }
                        Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; text: qsTranslate("Main", "Стек пока пуст"); font.weight: Font.DemiBold }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTranslate("Main", "Перетащите сюда пак из левого списка или нажмите стрелку.")
                            color: packPanels.theme.muted
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                        }
                    }
                }
                ColumnLayout {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 8
                    ActionButton {
                        objectName: "moveUp"
                        theme: packPanels.theme
                        glyph: "up"
                        enabled: !packPanels.locked && stack.currentIndex > 0
                        Accessible.name: qsTranslate("Main", "Повысить приоритет")
                        onClicked: {
                            const index = stack.currentIndex
                            if (packPanels.controller.MovePack(index, index - 1))
                                packPanels.selectPack(index - 1)
                        }
                    }
                    ActionButton {
                        objectName: "moveDown"
                        theme: packPanels.theme
                        glyph: "down"
                        enabled: !packPanels.locked && stack.currentIndex >= 0 && stack.currentIndex < stack.count - 1
                        Accessible.name: qsTranslate("Main", "Понизить приоритет")
                        onClicked: {
                            const index = stack.currentIndex
                            if (packPanels.controller.MovePack(index, index + 1))
                                packPanels.selectPack(index + 1)
                        }
                    }
                }
            }
        }
    }

}
