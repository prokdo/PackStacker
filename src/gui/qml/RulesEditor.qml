pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import PackStacker

Dialog {
    id: editor
    objectName: "rulesEditor"
    required property PackController controller
    required property UiTheme theme
    readonly property var strategyValues: ["standard", "high_priority", "low_priority", "language", "tag", "font", "atlas", "sounds"]
    readonly property var strategyLabels: [
        qsTranslate("Main", "Стандартная"), qsTranslate("Main", "Высший приоритет"),
        qsTranslate("Main", "Низший приоритет"), qsTranslate("Main", "Языковые ключи"),
        qsTranslate("Main", "Теги"), qsTranslate("Main", "Шрифты"),
        qsTranslate("Main", "Атласы"), qsTranslate("Main", "Звуки")
    ]
    anchors.centerIn: parent
    width: Math.min(900, parent.width - 60)
    height: Math.min(520, parent.height - 80)
    title: qsTranslate("Main", "Правила объединения")
    modal: true
    padding: 16
    palette.text: theme.text
    palette.windowText: theme.text
    palette.base: theme.surface
    palette.window: theme.background
    palette.button: theme.subtle
    palette.buttonText: theme.text
    palette.highlight: theme.selection
    palette.highlightedText: theme.text

    background: Rectangle {
        radius: 10
        color: editor.theme.surface
        border.color: editor.theme.line
    }
    header: Label {
        topPadding: 16
        leftPadding: 16
        rightPadding: 16
        text: editor.title
        color: editor.theme.text
        font.pixelSize: 18
        font.weight: Font.DemiBold
    }

    function loadDraft(rows: list<var>) {
        ruleDraft.clear()
        for (const row of rows)
            ruleDraft.append({match: row.match, strategy: row.strategy})
    }

    function draftValues(): list<var> {
        const rows = []
        for (let index = 0; index < ruleDraft.count; ++index) {
            const row = ruleDraft.get(index)
            rows.push({match: row.match, strategy: row.strategy})
        }
        return rows
    }

    onAboutToShow: {
        loadDraft(controller.rules)
        rulesMessage.text = ""
    }
    onApplied: {
        if (controller.ApplyRules(draftValues()))
            close()
        else
            rulesMessage.text = controller.status
    }

    ListModel { id: ruleDraft }

    Dialogs.FileDialog {
        id: importRulesFile
        objectName: "importRulesFile"
        title: qsTranslate("Main", "Импорт правил")
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: [qsTranslate("Main", "Правила JSON (*.json)")]
        onAccepted: {
            const result = editor.controller.ImportRules(selectedFile)
            if (result.ok) {
                editor.loadDraft(result.rules)
                rulesMessage.text = ""
            } else {
                rulesMessage.text = result.error || editor.controller.status
            }
        }
    }

    Dialogs.FileDialog {
        id: exportRulesFile
        objectName: "exportRulesFile"
        title: qsTranslate("Main", "Экспорт правил в новый JSON-файл")
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTranslate("Main", "Правила JSON (*.json)")]
        onAccepted: {
            if (!editor.controller.ExportRules(selectedFile, editor.draftValues()))
                rulesMessage.text = editor.controller.status
        }
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: editor.theme.muted
            font.pixelSize: 12
            text: qsTranslate("Main", "Последнее совпадение побеждает. * — внутри каталога, ** — включая подкаталоги. pack.mcmeta создаётся автоматически.")
        }
        ListView {
            objectName: "ruleList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: ruleDraft
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                id: ruleRow
                required property int index
                required property string match
                required property string strategy
                width: ListView.view.width
                spacing: 6
                EntryField {
                    objectName: "ruleMatch" + ruleRow.index
                    theme: editor.theme
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: ruleRow.match
                    placeholderText: "assets/**/lang/*.json"
                    Accessible.name: qsTranslate("Main", "Шаблон пути")
                    onTextEdited: ruleDraft.setProperty(ruleRow.index, "match", text)
                }
                ComboBox {
                    id: strategyChoice
                    objectName: "ruleStrategy" + ruleRow.index
                    Layout.preferredWidth: 180
                    implicitHeight: 34
                    hoverEnabled: true
                    font.pixelSize: 13
                    palette.dark: editor.theme.muted
                    palette.mid: editor.theme.line
                    Accessible.name: qsTranslate("Main", "Стратегия")
                    model: editor.strategyLabels
                    currentIndex: editor.strategyValues.indexOf(ruleRow.strategy)
                    onActivated: ruleDraft.setProperty(ruleRow.index, "strategy", editor.strategyValues[currentIndex])
                    background: Rectangle {
                        radius: 6
                        color: strategyChoice.down || strategyChoice.hovered ? editor.theme.hover : editor.theme.subtle
                        border.color: strategyChoice.visualFocus ? editor.theme.accent : editor.theme.line
                    }
                }
                ActionButton {
                    objectName: "ruleUp" + ruleRow.index
                    theme: editor.theme
                    glyph: "up"
                    quiet: true
                    Accessible.name: qsTranslate("Main", "Переместить вверх")
                    enabled: ruleRow.index > 0
                    onClicked: ruleDraft.move(ruleRow.index, ruleRow.index - 1, 1)
                }
                ActionButton {
                    objectName: "ruleDown" + ruleRow.index
                    theme: editor.theme
                    glyph: "down"
                    quiet: true
                    Accessible.name: qsTranslate("Main", "Переместить вниз")
                    enabled: ruleRow.index + 1 < ruleDraft.count
                    onClicked: ruleDraft.move(ruleRow.index, ruleRow.index + 1, 1)
                }
                ActionButton {
                    objectName: "ruleDelete" + ruleRow.index
                    theme: editor.theme
                    quiet: true
                    text: qsTranslate("Main", "Удалить")
                    onClicked: ruleDraft.remove(ruleRow.index)
                }
            }
        }
        Label {
            id: rulesMessage
            objectName: "rulesMessage"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: text !== ""
            color: editor.theme.error
        }
        RowLayout {
            Layout.fillWidth: true
            ActionButton {
                objectName: "addRule"
                theme: editor.theme
                text: qsTranslate("Main", "Добавить правило")
                enabled: ruleDraft.count < 256
                onClicked: ruleDraft.append({match: "assets/**", strategy: "standard"})
            }
            ActionButton {
                theme: editor.theme
                quiet: true
                text: qsTranslate("Main", "Импорт…")
                onClicked: importRulesFile.open()
            }
            ActionButton {
                theme: editor.theme
                quiet: true
                text: qsTranslate("Main", "Экспорт…")
                onClicked: exportRulesFile.open()
            }
            Item { Layout.fillWidth: true }
            ActionButton {
                objectName: "cancelRuleDraft"
                theme: editor.theme
                quiet: true
                text: qsTranslate("Main", "Отменить")
                onClicked: editor.reject()
            }
            ActionButton {
                objectName: "applyRuleDraft"
                theme: editor.theme
                primary: true
                text: qsTranslate("Main", "Применить")
                onClicked: editor.applied()
            }
        }
    }
}
