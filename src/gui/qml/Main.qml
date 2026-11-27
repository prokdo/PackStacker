pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PackStacker

ApplicationWindow {
    id: window
    required property PackController controller
    readonly property bool locked: controller.busy || controller.scanning
    property bool closeWhenIdle: false
    readonly property string configurationKey: JSON.stringify([controller.packs, packType.currentIndex,
        targetFormat.text, output.path, output.format, output.description, controller.rules])
    property string completedConfigurationKey: ""
    property string cancelledConfigurationKey: ""
    property string cancellationMessage: ""
    property string failedConfigurationKey: ""
    property int validationRevision: 0
    property int catalogRevision: 0
    readonly property var topPack: controller.selectedPacks.length > 0 ? controller.selectedPacks[0] : null
    readonly property string automaticFormat: topPack && !topPack.error ? topPack.autoFormat || "" : ""
    readonly property alias outputPathAutomatic: output.outputPathAutomatic
    readonly property alias descriptionAutomatic: output.descriptionAutomatic
    readonly property string automaticDescription: topPack && !topPack.error ? topPack.outputDescription || "" : ""
    readonly property string effectiveFormat: targetFormat.text === "" ? automaticFormat : targetFormat.text
    readonly property var validation: {
        const revision = validationRevision
        return controller.ValidateInputs(effectiveFormat, output.path)
    }
    readonly property string targetError: targetFormat.text === "" && topPack && !automaticFormat
        ? qsTr("У верхнего пака нет однозначного формата. Укажите его вручную.") : validation.target
    readonly property string readiness: {
        const revision = catalogRevision
        if (controller.packs.length === 0)
            return qsTr("Добавьте паки в стек")
        for (const entry of controller.selectedPacks) {
            if (entry.error)
                return qsTr("В стеке есть пак с ошибкой чтения")
            const warning = controller.PackTypeWarning(entry.path, packType.currentIndex, effectiveFormat)
            if (warning !== "")
                return entry.name + ": " + warning
        }
        if (targetError !== "")
            return targetError
        if (validation.output !== "")
            return validation.output
        return ""
    }
    readonly property bool completed: controller.resultPath !== "" && completedConfigurationKey === configurationKey
    readonly property bool currentFailure: controller.failed && failedConfigurationKey === configurationKey
    visible: true
    width: 1280
    height: 720
    minimumWidth: 1000
    minimumHeight: 640
    title: "PackStacker"
    color: theme.background
    font.pixelSize: 13
    palette.text: theme.text
    palette.windowText: theme.text
    palette.base: theme.surface
    palette.window: theme.background
    palette.button: theme.surface
    palette.buttonText: theme.text
    palette.highlight: theme.accent

    UiTheme { id: theme }
    readonly property UiTheme designTheme: theme

    Component.onCompleted: {
        controller.StateChanged.connect(handleStateChanged)
        controller.CatalogChanged.connect(handleCatalogChanged)
        controller.Finished.connect(handleFinished)
        if (controller.packs.length > 0)
            controller.RefreshCatalog()
    }

    Component.onDestruction: {
        controller.StateChanged.disconnect(handleStateChanged)
        controller.CatalogChanged.disconnect(handleCatalogChanged)
        controller.Finished.disconnect(handleFinished)
    }

    onActiveChanged: validationRevision++

    onClosing: function(close) {
        if (locked) {
            close.accepted = false
            closeConfirmation.open()
        }
    }

    function handleStateChanged() {
        window.validationRevision++
        if (window.controller.busy)
            window.cancelledConfigurationKey = ""
        if (window.controller.failed)
            window.failedConfigurationKey = window.configurationKey
        if (window.closeWhenIdle && !window.locked)
            window.close()
    }

    function handleCatalogChanged() {
        window.catalogRevision++
        window.validationRevision++
        Qt.callLater(packPanels.restoreSelection)
    }

    function handleFinished(success) {
        window.completedConfigurationKey = success ? window.configurationKey : ""
        if (!success && !window.controller.failed) {
            window.cancelledConfigurationKey = window.configurationKey
            window.cancellationMessage = window.controller.status
        }
        window.validationRevision++
    }

    RulesEditor {
        id: rulesEditor
        controller: window.controller
        theme: window.designTheme
    }
    Dialog {
        id: about
        objectName: "aboutDialog"
        anchors.centerIn: parent
        title: qsTr("О программе")
        modal: true
        standardButtons: Dialog.Close
        Label {
            objectName: "aboutVersion"
            text: "PackStacker " + Qt.application.version
        }
    }
    Dialog {
        id: closeConfirmation
        objectName: "closeConfirmation"
        anchors.centerIn: parent
        title: qsTr("Отменить операцию и закрыть окно?")
        modal: true
        standardButtons: Dialog.NoButton
        footer: DialogButtonBox {
            Button { text: qsTr("Отменить и закрыть"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { text: qsTr("Продолжить работу"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        Label {
            text: qsTr("Приложение дождётся остановки фоновой работы.\nУже опубликованный результат останется на диске.")
        }
        onAccepted: {
            window.closeWhenIdle = true
            if (window.locked)
                window.controller.Cancel()
            else
                window.close()
        }
    }
    Dialog {
        id: statusDetails
        anchors.centerIn: parent
        title: qsTr("Подробности операции")
        width: Math.min(680, window.width - 80)
        height: 300
        modal: true
        standardButtons: Dialog.Close
        ScrollView {
            anchors.fill: parent
            TextArea {
                text: window.controller.status
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
            }
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "appHeader"
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: "transparent"
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.line }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                spacing: 12
                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 9
                    color: theme.selection
                    Glyph { anchors.centerIn: parent; width: 23; height: 23; color: theme.accent }
                }
                Label { text: "PackStacker"; font.pixelSize: 19; font.weight: Font.DemiBold }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.leftMargin: 6; Layout.rightMargin: 6; color: theme.line }
                Item { Layout.fillWidth: true }
                ActionButton {
                    objectName: "rulesButton"
                    theme: window.designTheme
                    quiet: true
                    text: qsTr("Правила…")
                    enabled: !window.locked
                    onClicked: rulesEditor.open()
                }
                ActionButton {
                    objectName: "aboutButton"
                    theme: window.designTheme
                    quiet: true
                    text: qsTr("О программе")
                    onClicked: about.open()
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            Layout.maximumHeight: implicitHeight
            Layout.topMargin: 12
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            columns: 3
            columnSpacing: 0
            rowSpacing: 2
            RowLayout {
                Layout.row: 0
                Layout.column: 0
                Layout.rowSpan: 2
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                Layout.minimumHeight: 34
                Layout.maximumHeight: 34
                spacing: 12
                Label { objectName: "packTypeLabel"; text: qsTr("Тип пака"); color: theme.muted; font.pixelSize: 12 }
                Choice {
                    id: packType
                    objectName: "packType"
                    theme: window.designTheme
                    options: [qsTr("Ресурсы"), qsTr("Данные")]
                    enabled: !window.locked
                }
                Item { Layout.fillWidth: true }
            }
            Item {
                Layout.row: 0
                Layout.column: 1
                Layout.rowSpan: 2
                Layout.minimumWidth: 54
                Layout.maximumWidth: 54
            }
            RowLayout {
                Layout.row: 0
                Layout.column: 2
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                Layout.minimumHeight: 34
                Layout.maximumHeight: 34
                Label { objectName: "targetFormatLabel"; text: qsTr("Целевой pack format"); color: theme.muted; font.pixelSize: 12 }
                Glyph {
                    name: "info"
                    Layout.preferredWidth: 15
                    Layout.preferredHeight: 15
                    color: theme.muted
                    ToolTip.visible: helpHover.hovered
                    ToolTip.text: qsTr("Это формат пака, не версия Minecraft. Пустое поле — авто из верхнего пака. Конвертация версий не выполняется.")
                    HoverHandler { id: helpHover }
                }
                Item { Layout.fillWidth: true }
                EntryField {
                    id: targetFormat
                    objectName: "targetFormat"
                    theme: window.designTheme
                    Layout.preferredWidth: 150
                    placeholderText: window.automaticFormat ? qsTr("Авто: %1").arg(window.automaticFormat) : qsTr("Авто")
                    invalid: window.targetError !== "" && (text !== "" || !!window.topPack)
                    enabled: !window.locked
                    Accessible.name: qsTr("Целевой pack format")
                }
            }
            Label {
                objectName: "targetValidation"
                Layout.row: 1
                Layout.column: 2
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 1
                Layout.minimumHeight: implicitHeight
                Layout.alignment: Qt.AlignTop
                text: targetFormat.invalid ? window.targetError
                    : targetFormat.text === "" ? qsTr("Авто — из верхнего пака. Для ручного выбора введите число.")
                                               : qsTr("Ручной выбор. Очистите поле, чтобы вернуть авто.")
                color: targetFormat.invalid ? theme.error : theme.muted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        PackPanels {
            catalogRevision: window.catalogRevision
            selectedType: packType.currentIndex
            targetFormat: window.effectiveFormat
            id: packPanels
            controller: window.controller
            theme: window.designTheme
            locked: window.locked
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.topMargin: 8
            Layout.bottomMargin: 14
        }

        OutputSettings {
            id: output
            controller: window.controller
            theme: window.designTheme
            locked: window.locked
            automaticDescription: window.automaticDescription
            outputError: window.validation.output
            completed: window.completed
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            onPathEditingFinished: window.validationRevision++
        }

        ExportBar {
            theme: window.designTheme
            locked: window.locked
            failed: window.currentFailure
            completed: window.completed
            cancelling: window.controller.cancelling
            ready: window.readiness === ""
            message: window.locked || window.currentFailure ? window.controller.status.split('\n')[0]
                : window.completed ? qsTr("Объединение завершено")
                : window.cancelledConfigurationKey === window.configurationKey ? window.cancellationMessage
                : window.readiness || qsTr("Готово к запуску. Совместимость проверится при объединении.")
            details: window.controller.status
            resultPath: window.controller.resultPath
            Layout.fillWidth: true
            onStartRequested: {
                window.validationRevision++
                if (window.readiness === "")
                    window.controller.Start(packType.currentIndex, window.effectiveFormat,
                                            output.path, output.format, output.description)
            }
            onCancelRequested: window.controller.Cancel()
            onResultRequested: window.controller.OpenResultFolder()
            onDetailsRequested: statusDetails.open()
        }
    }
}
