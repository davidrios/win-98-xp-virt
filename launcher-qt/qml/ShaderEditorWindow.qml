// The shader profile editor (doc 07): a slider per preset parameter and
// the live preview beside them — `launcher/src/shader_manager.rs`'s
// editor half.
//
// Its own top-level window, and a big one: the preview is the whole
// point, and a window the user can drag out to their screen's width
// beats the in-window "Fullscreen" toggle the egui build had to grow.
// The preview itself is rendered by `shader-chain` on this process's own
// windowless wgpu device and arrives through a file; `src/preview.rs`
// explains why, and what the egui build gets for free instead.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property ShaderEditor editor
    required property string profilesDir

    signal changed()

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: editorBody

    title: qsTr("Shader profile")
    width: 1200
    height: 780
    minimumWidth: 720
    minimumHeight: 420
    flags: Qt.Dialog
    color: palette.window

    // Closing the window is cancelling the edit; the flag drives the
    // window in both directions from `Main.qml`.
    onVisibleChanged: if (!visible && editor.open) editor.open = false

    /// Open on a given preset and preview image — the headless
    /// screenshot path (`src/qt/diag.rs`).
    function editPreset(preset, image) {
        editor.newProfile()
        editor.presetPath = preset
        editor.previewImage = image
        editor.reparse()
        rerender()
    }

    // Re-render whenever anything the picture depends on moves. A
    // binding, not a call after every slider: QML already knows what the
    // preview depends on, which is the one place this port needs less
    // bookkeeping than the immediate-mode version.
    function rerender() {
        if (editor.open && editor.presetPath !== "" && editor.previewImage !== "")
            editor.render(previewArea.width, previewArea.height)
    }

    Timer {
        // Coalesces a slider drag into one render per frame-ish rather
        // than one per pixel of travel.
        id: renderDebounce
        interval: 16
        onTriggered: root.rerender()
    }

    Timer {
        interval: 300
        repeat: true
        running: root.visible && root.editor.downloadState.startsWith("running")
        onTriggered: root.editor.pollDownload()
    }


    // The grab target for the headless screenshot path: a QML-declared
    // item (Qt refuses `grabToImage` on anything the engine did not
    // create) that is opaque (a bare layout grabs with a transparent
    // background and dark-on-nothing text). On screen it is just the
    // window's own colour.
    Rectangle {
        id: editorBody
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: editorBodyLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Name"); Layout.minimumWidth: 150 }
                TextField {
                    Layout.fillWidth: true
                    text: root.editor.name
                    selectByMouse: true
                    onTextChanged: root.editor.name = text
                }
            }

            PathField {
                Layout.fillWidth: true
                label: qsTr("Preset (.slangp)")
                nameFilter: qsTr("Shader presets (*.slangp)")
                // "Browse…" on an empty field opens in the preset collection
                // rather than the OS default: a `.slangp` lives in a
                // checkout's `third_party/` or in a downloaded copy under a
                // data directory, and neither is anywhere a person would
                // navigate to by hand.
                emptyDir: root.editor.presetsDir
                value: root.editor.presetPath
                onValueChanged: {
                    root.editor.presetPath = value
                    root.editor.reparse()
                    root.rerender()
                }
            }

            PresetCollection { editor: root.editor }

            Label {
                Layout.fillWidth: true
                visible: root.editor.parseError !== ""
                text: qsTr("Couldn't read this preset's parameters: %1").arg(root.editor.parseError)
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            MenuSeparator { Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                // A fixed-width controls column; the rest of the window goes
                // to the preview, so making the window wider makes the
                // *picture* bigger, which is the point of a preview.
                ColumnLayout {
                    Layout.preferredWidth: 320
                    Layout.fillHeight: true
                    spacing: 4

                    Label {
                        visible: root.editor.count === 0
                        text: qsTr("Pick a preset to see its parameters.")
                        opacity: 0.7
                    }

                    ListView {
                        id: params
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.editor
                        spacing: 2
                        ScrollBar.vertical: ScrollBar {}

                        delegate: ColumnLayout {
                            id: paramRow
                            required property int index
                            required property string paramId
                            required property string description
                            required property real minimum
                            required property real maximum
                            required property real step
                            required property real defaultValue
                            required property real value
                            required property bool overridden

                            width: params.width - 12
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                CheckBox {
                                    checked: paramRow.overridden
                                    onToggled: {
                                        root.editor.setOverride(paramRow.index, checked)
                                        root.rerender()
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        text: qsTr("%1  %2").arg(paramRow.paramId)
                                            .arg(slider.value.toFixed(3))
                                        font.pixelSize: 11
                                        opacity: paramRow.overridden ? 1.0 : 0.55
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Slider {
                                        id: slider
                                        Layout.fillWidth: true
                                        enabled: paramRow.overridden
                                        from: paramRow.minimum
                                        to: paramRow.maximum
                                        // Step only where the user is
                                        // actually editing: several presets
                                        // have defaults off their own step
                                        // grid (crt-lottes: warpX 0.031, step
                                        // 0.01), and snapping an
                                        // un-overridden one would silently
                                        // change what the preview shows.
                                        stepSize: paramRow.overridden ? paramRow.step : 0
                                        value: paramRow.value
                                        onMoved: {
                                            root.editor.setValue(paramRow.index, value)
                                            renderDebounce.restart()
                                        }
                                    }
                                }
                            }
                            Label {
                                visible: paramRow.description !== ""
                                text: paramRow.description
                                font.pixelSize: 10
                                opacity: 0.55
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 28
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6

                    PathField {
                        Layout.fillWidth: true
                        label: qsTr("Preview image")
                        nameFilter: qsTr("Images (*.png *.jpg *.jpeg *.bmp)")
                        value: root.editor.previewImage
                        onValueChanged: {
                            root.editor.previewImage = value
                            root.rerender()
                        }
                    }

                    // Black behind, the frame centred at exactly the size it
                    // rendered at — "how it'll really look in the player",
                    // not an image stretched to fit.
                    Rectangle {
                        id: previewArea
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "black"
                        clip: true

                        onWidthChanged: renderDebounce.restart()
                        onHeightChanged: renderDebounce.restart()

                        Image {
                            anchors.centerIn: parent
                            width: root.editor.previewWidth
                            height: root.editor.previewHeight
                            source: root.editor.previewSource
                            cache: false
                            smooth: false
                            visible: root.editor.previewSource !== ""
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: root.editor.previewImage === ""
                            color: "#909090"
                            text: qsTr("Pick a screenshot to preview the shader on it.")
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.editor.error !== ""
                text: root.editor.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            RowLayout {
                spacing: 8
                Button {
                    text: qsTr("Save")
                    onClicked: {
                        if (root.editor.save(root.profilesDir)) {
                            root.profiles.refresh()
                            root.changed()
                        }
                    }
                }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.editor.open = false
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    // The preset-collection row, shown on both screens because that is
    // where each question is asked: the list is where someone discovers
    // they have no shaders at all, the editor is where an empty preset
    // field stops them mid-profile.
}
