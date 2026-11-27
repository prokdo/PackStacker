pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import PackStacker

Rectangle {
    id: settings
    objectName: "outputSettings"
    required property PackController controller
    required property UiTheme theme
    required property bool locked
    required property string automaticDescription
    required property string outputError
    required property bool completed
    readonly property alias path: outputPath.text
    readonly property alias format: outputFormat.currentIndex
    readonly property alias description: descriptionField.text
    property bool outputPathAutomatic: true
    property bool descriptionAutomatic: true
    readonly property string automaticOutputPath: controller.DefaultOutputPath(controller.sourceDirectory, format)
    signal pathEditingFinished()

    implicitHeight: resultLayout.implicitHeight + 30
    Layout.minimumHeight: implicitHeight
    Layout.maximumHeight: implicitHeight
    color: settings.theme.surface
    radius: 9
    border.color: settings.theme.line
    function updateAutomaticDefaults() {
        if (outputPathAutomatic && outputPath)
            outputPath.text = automaticOutputPath
        if (descriptionAutomatic && descriptionField)
            descriptionField.text = automaticDescription
    }

    onAutomaticOutputPathChanged: updateAutomaticDefaults()
    onAutomaticDescriptionChanged: updateAutomaticDefaults()

    Component.onCompleted: updateAutomaticDefaults()

    Dialogs.FileDialog {
        id: outputFile
        title: qsTranslate("Main", "Выбрать НОВЫЙ выходной ZIP")
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "zip"
        nameFilters: [qsTranslate("Main", "ZIP-паки (*.zip)")]
        onAccepted: {
            settings.outputPathAutomatic = false
            outputPath.text = settings.controller.LocalPath(selectedFile)
        }
    }
    Dialogs.FolderDialog {
        id: outputParent
        title: qsTranslate("Main", "Выбрать родительский каталог; имя новой папки можно изменить")
        onAccepted: {
            settings.outputPathAutomatic = false
            outputPath.text = settings.controller.LocalPath(selectedFolder) + "/combined-pack"
        }
    }

    ColumnLayout {
        id: resultLayout
        anchors.fill: parent
        anchors.margins: 15
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTranslate("Main", "Результат"); font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            Label { text: qsTranslate("Main", "Исходные паки не изменяются. Перезапись запрещена."); color: settings.theme.muted; font.pixelSize: 11 }
        }
        GridLayout {
            objectName: "outputFields"
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 12
            rowSpacing: 2
            Label {
                objectName: "outputPathLabel"
                Layout.row: 0
                Layout.column: 1
                Layout.columnSpan: 2
                text: qsTranslate("Main", "Укажите путь нового результата.")
                color: settings.theme.muted
                font.pixelSize: 11
            }
            Choice {
                id: outputFormat
                objectName: "outputFormat"
                theme: settings.theme
                vertical: true
                Layout.row: 1
                Layout.column: 0
                Layout.rowSpan: 4
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                options: [qsTranslate("Main", "ZIP"), qsTranslate("Main", "Папка")]
                enabled: !settings.locked
            }
            EntryField {
                id: outputPath
                objectName: "outputPath"
                theme: settings.theme
                Layout.row: 1
                Layout.column: 1
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: outputFormat.currentIndex === 0
                    ? qsTranslate("Main", "По умолчанию: каталог слева / combined-pack.zip")
                    : qsTranslate("Main", "По умолчанию: каталог слева / combined-pack")
                toolTip: qsTranslate("Main", "По умолчанию: %1 в каталоге слева. После ручного изменения путь больше не подставляется автоматически. Перезапись запрещена.")
                    .arg(outputFormat.currentIndex === 0 ? "combined-pack.zip" : "combined-pack")
                enabled: !settings.locked
                invalid: text !== "" && settings.outputError !== "" && !settings.completed
                Accessible.name: qsTranslate("Main", "Выходной путь")
                onTextEdited: settings.outputPathAutomatic = false
                onEditingFinished: settings.pathEditingFinished()
            }
            ActionButton {
                objectName: "browseOutput"
                theme: settings.theme
                Layout.row: 1
                Layout.column: 2
                glyph: "open"
                quiet: true
                enabled: !settings.locked
                Accessible.name: qsTranslate("Main", "Выбрать выходной путь")
                onClicked: outputFormat.currentIndex === 0 ? outputFile.open() : outputParent.open()
            }
            Label {
                objectName: "outputValidation"
                Layout.row: 2
                Layout.column: 1
                Layout.columnSpan: 2
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text !== "" && outputPath.text !== ""
                Layout.minimumHeight: implicitHeight
                text: settings.completed ? qsTranslate("Main", "Результат создан. Для новой сборки выберите другой путь.") : settings.outputError
                color: outputPath.invalid ? settings.theme.error : settings.theme.muted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Label {
                objectName: "descriptionLabel"
                Layout.row: 3
                Layout.column: 1
                Layout.columnSpan: 2
                text: qsTranslate("Main", "Описание результата")
                color: settings.theme.muted
                font.pixelSize: 11
            }
            EntryField {
                id: descriptionField
                objectName: "description"
                theme: settings.theme
                placeholderText: qsTranslate("Main", "По умолчанию: описание верхнего пака")
                toolTip: qsTranslate("Main", "По умолчанию описание берётся из верхнего пака и обновляется при смене приоритета, пока вы не измените его вручную.")
                Layout.row: 4
                Layout.column: 1
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                enabled: !settings.locked
                Accessible.name: qsTranslate("Main", "Описание результата")
                onTextEdited: settings.descriptionAutomatic = false
            }
        }
    }
}
