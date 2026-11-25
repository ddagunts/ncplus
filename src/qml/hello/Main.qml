import QtQuick

// Hello World on an LG NetCast TV. Remote: UP/DOWN pick a colour, OK pulses, CH- (or Escape) quits.
Window {
    id: win
    width: 1280; height: 720
    visible: true
    color: "#0e1220"
    title: "QML Hello"

    property var themes: [
        { name: "Ocean",  a: "#1d4e89", b: "#00b2ca" },
        { name: "Sunset", a: "#c0392b", b: "#f39c12" },
        { name: "Forest", a: "#1e5631", b: "#a4de02" },
        { name: "Violet", a: "#4a148c", b: "#e040fb" }
    ]

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: win.themes[list.currentIndex].a; Behavior on color { ColorAnimation { duration: 400 } } }
            GradientStop { position: 1; color: win.themes[list.currentIndex].b; Behavior on color { ColorAnimation { duration: 400 } } }
        }
    }

    // floating circles
    Repeater {
        model: 14
        Rectangle {
            required property int index
            width: 40 + (index * 37) % 120; height: width; radius: width / 2
            color: "white"; opacity: 0.08
            x: (index * 211) % win.width
            NumberAnimation on y {
                from: win.height + 150; to: -150; loops: Animation.Infinite
                duration: 7000 + (index * 1300) % 9000
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 24
        Text {
            id: hello
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Hello, NetCast!"
            color: "white"; font.pixelSize: 96; font.bold: true
            SequentialAnimation on scale {
                id: pulse; running: false
                NumberAnimation { to: 1.15; duration: 120; easing.type: Easing.OutQuad }
                NumberAnimation { to: 1.0; duration: 300; easing.type: Easing.OutBounce }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Qt " + Qt.application.version + "  ·  QML on Linux 3.0 · PowerVR SGX544 · GLES2"
            color: "#e8ecf8"; font.pixelSize: 26
        }
        ListView {
            id: list
            anchors.horizontalCenter: parent.horizontalCenter
            width: 560; height: 4 * 64
            model: win.themes
            focus: true
            interactive: false
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: list.width; height: 56; radius: 12
                color: ListView.isCurrentItem ? "#ffffffdd" : "#ffffff33"
                Behavior on color { ColorAnimation { duration: 150 } }
                Text {
                    anchors.centerIn: parent
                    text: modelData.name
                    font.pixelSize: 30
                    color: parent.ListView.isCurrentItem ? "#101420" : "white"
                }
            }
            spacing: 8
            Keys.onReturnPressed: pulse.restart()
            Keys.onEnterPressed: pulse.restart()
            Keys.onEscapePressed: Qt.quit()
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_MediaPrevious || e.key === Qt.Key_Back || e.key === Qt.Key_Exit) Qt.quit()
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "UP/DOWN colour   OK pulse   BACK quit"
            color: "#c8d0e8"; font.pixelSize: 22
        }
    }
}
