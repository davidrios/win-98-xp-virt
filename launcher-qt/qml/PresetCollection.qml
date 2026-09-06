// The preset-collection row, shown on both shader windows because that
// is where each question is asked: the list is where someone discovers
// they have no shaders at all, the editor is where an empty preset field
// stops them mid-profile. Nothing at all once a collection is on disk.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

ColumnLayout {
    required property ShaderEditor editor
    Layout.fillWidth: true
    spacing: 2
    visible: editor.presetsDir === "" || editor.downloadState !== ""

    RowLayout {
        spacing: 8
        visible: editor.downloadState.startsWith("running")
        BusyIndicator { implicitWidth: 18; implicitHeight: 18; running: visible }
        Label {
            text: qsTr("Downloading shader presets… %1 MB")
                .arg(editor.downloadState.substring("running:".length))
        }
    }
    Label {
        Layout.fillWidth: true
        visible: editor.downloadState.startsWith("failed")
        wrapMode: Text.Wrap
        color: "#d04040"
        text: qsTr("Couldn't download the shader presets: %1")
            .arg(editor.downloadState.substring("failed:".length))
    }
    Label {
        visible: editor.presetsDir === "" && editor.downloadState === ""
        text: qsTr("No shader presets on this machine — a profile needs a .slangp to build on.")
        opacity: 0.75
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Button {
        visible: editor.presetsDir === "" && !editor.downloadState.startsWith("running")
        text: editor.downloadState.startsWith("failed")
            ? qsTr("Try again")
            : qsTr("Download presets (~50 MB)")
        onClicked: editor.downloadPresets()
    }
}
