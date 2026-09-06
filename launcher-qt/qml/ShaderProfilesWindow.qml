// The shader profile list (doc 07): New / Edit / Delete over
// `shader_library`. The editor it opens is its own window
// (`ShaderEditorWindow.qml`) rather than a second mode of this one — a
// real window cannot be reliably resized once the window manager has
// mapped it, and the two want very different sizes.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

Window {
    id: root

    // Typed, not `var`: a `var` holding a QObject gives QML no property
    // metadata, so a binding on one of its properties is read once and
    // never re-evaluated. The types are registered by `#[qml_element]`,
    // so naming them here costs nothing.
    required property ProfileModel profiles
    required property ShaderEditor editor

    signal changed()

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: listBody

    title: qsTr("Shader profiles")
    width: 660
    height: 340
    minimumWidth: 480
    minimumHeight: 240
    flags: Qt.Dialog
    color: palette.window

    onVisibleChanged: if (!visible) root.changed()


    // The grab target for the headless screenshot path: a QML-declared
    // item (Qt refuses `grabToImage` on anything the engine did not
    // create) that is opaque (a bare layout grabs with a transparent
    // background and dark-on-nothing text). On screen it is just the
    // window's own colour.
    Rectangle {
        id: listBody
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: listBodyLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            Frame {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 0
                // See `Main.qml`: the style's Frame paints a border only.
                background: Rectangle {
                    color: palette.base
                    border.color: palette.mid
                }

                ListView {
                    id: profileList
                    anchors.fill: parent
                    clip: true
                    model: root.profiles
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: profileRow
                        required property int index
                        required property string name
                        required property string preset

                        width: profileList.width
                        implicitHeight: 40
                        color: index % 2 ? palette.base : palette.alternateBase

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Label { text: profileRow.name; Layout.preferredWidth: 170; elide: Text.ElideRight }
                            Label {
                                text: profileRow.preset
                                Layout.fillWidth: true
                                elide: Text.ElideLeft
                                opacity: 0.7
                            }
                            Button {
                                text: qsTr("Edit…")
                                onClicked: root.editor.edit(root.profiles.pathAt(profileRow.index))
                            }
                            Button {
                                text: qsTr("Delete")
                                onClicked: { root.profiles.deleteAt(profileRow.index); root.changed() }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.profiles.count === 0
                        opacity: 0.7
                        text: qsTr("No shader profiles yet.")
                    }
                }
            }

            RowLayout {
                spacing: 8
                Button {
                    text: qsTr("New profile…")
                    onClicked: root.editor.newProfile()
                }
                Item { Layout.fillWidth: true }
            }

            PresetCollection { editor: root.editor }
        }
    }
}
