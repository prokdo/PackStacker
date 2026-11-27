import QtQuick
import QtQuick.Shapes

Item {
    id: root
    property string name: "layers"
    property color color: "#677384"
    implicitWidth: 18
    implicitHeight: 18
    readonly property string path: ({
        "layers": "M3 7 L12 3 L21 7 L12 11 Z M3 12 L12 16 L21 12 M3 17 L12 21 L21 17",
        "folder": "M3 7 L3 5 L9 5 L11 7 L21 7 L21 20 L3 20 Z M3 10 L21 10",
        "open": "M3 18 L3 5 L9 5 L11 7 L20 7 L20 10 M3 19 L6 11 L22 11 L19 19 Z",
        "file": "M6 3 L14 3 L18 7 L18 21 L6 21 Z M14 3 L14 8 L18 8 M9 12 L15 12 M9 16 L15 16",
        "save": "M4 3 L17 3 L20 6 L20 21 L4 21 Z M8 3 L8 9 L16 9 L16 3 M8 21 L8 13 L16 13 L16 21",
        "right": "M5 12 L19 12 M13 6 L19 12 L13 18",
        "left": "M19 12 L5 12 M11 6 L5 12 L11 18",
        "up": "M12 19 L12 5 M6 11 L12 5 L18 11",
        "down": "M12 5 L12 19 M6 13 L12 19 L18 13",
        "refresh": "M20 8 C17 1 7 2 4 8 C0 17 14 25 20 16 M20 3 L20 8 L15 8",
        "sun": "M12 8 A4 4 0 1 1 12 16 A4 4 0 1 1 12 8 M12 2 L12 4 M12 20 L12 22 M2 12 L4 12 M20 12 L22 12 M5 5 L6 6 M18 18 L19 19 M5 19 L6 18 M18 6 L19 5",
        "lock": "M5 10 L19 10 L19 21 L5 21 Z M8 10 L8 7 C8 2 16 2 16 7 L16 10 M12 14 L12 17",
        "info": "M12 3 A9 9 0 1 1 12 21 A9 9 0 1 1 12 3 M12 11 L12 17 M12 7 L12 7.2",
        "warning": "M12 3 L22 21 L2 21 Z M12 9 L12 14 M12 17 L12 17.2"
    })[name] || ""
    Shape {
        width: 24
        height: 24
        scale: Math.min(root.width, root.height) / 24
        transformOrigin: Item.TopLeft
        antialiasing: true
        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.7
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.path }
        }
    }
}
