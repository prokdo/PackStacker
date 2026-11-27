import QtQuick

QtObject {
    readonly property SystemPalette systemPalette: SystemPalette { colorGroup: SystemPalette.Active }
    readonly property bool dark: Application.styleHints.colorScheme === Qt.ColorScheme.Dark
        || (Application.styleHints.colorScheme === Qt.ColorScheme.Unknown && systemPalette.window.hslLightness < 0.5)
    readonly property color background: dark ? "#181818" : "#f4f6f8"
    readonly property color surface: dark ? "#212121" : "#ffffff"
    readonly property color subtle: dark ? "#272727" : "#f7f8fa"
    readonly property color text: dark ? "#ededed" : "#202731"
    readonly property color muted: dark ? "#ababab" : "#677384"
    readonly property color line: dark ? "#3a3a3a" : "#e0e5eb"
    readonly property color hover: dark ? "#303030" : "#f1f4f8"
    readonly property color accent: dark ? "#8bb3ff" : "#2563eb"
    readonly property color selection: dark ? "#303030" : "#eef4ff"
    readonly property color selectionBorder: dark ? "#626262" : "#aac5fb"
    readonly property color error: dark ? "#ff9292" : "#c73939"
    readonly property color warning: dark ? "#e6bc72" : "#8a5c10"
    readonly property color success: dark ? "#72caa4" : "#27815f"
}
