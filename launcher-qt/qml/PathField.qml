// A labelled path field with a "Browse…" button — the Qt equivalent of
// `launcher/src/filepicker.rs`'s `path_field`.
//
// Typing directly is still allowed (a path the user already knows, or one
// on a mount the picker can't reach); the button is a convenience, not
// the only way in. `FileDialog` is Qt's own, which on Linux is the XDG
// desktop portal — the same backend the egui build reaches through
// `rfd`, with no extra dependency.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

RowLayout {
    id: root

    property alias label: caption.text
    property string value
    /// e.g. "Disc images (*.iso *.cue *.ccd *.mds)". "All files (*)" is
    /// always offered alongside: a filter that hides the file someone is
    /// looking for is worse than no filter.
    property string nameFilter: ""
    /// Where the dialog opens when the field is still empty — the shader
    /// preset field points it at the preset collection, which is
    /// otherwise buried in a data directory nobody would navigate to.
    property string emptyDir: ""

    spacing: 8

    Label {
        id: caption
        Layout.minimumWidth: 150
        Layout.alignment: Qt.AlignVCenter
    }

    TextField {
        id: field
        Layout.fillWidth: true
        text: root.value
        selectByMouse: true
        onTextChanged: root.value = text
    }

    Button {
        text: qsTr("Browse…")
        onClicked: dialog.open()
    }

    FileDialog {
        id: dialog
        nameFilters: root.nameFilter === ""
            ? [qsTr("All files (*)")]
            : [root.nameFilter, qsTr("All files (*)")]
        // The field's own value wins over the caller's suggestion:
        // re-opening browses from where it already points.
        currentFolder: {
            if (root.value !== "")
                return "file://" + root.value.replace(/\/[^\/]*$/, "")
            if (root.emptyDir !== "")
                return "file://" + root.emptyDir
            return ""
        }
        onAccepted: root.value = selectedFile.toString().replace(/^file:\/\//, "")
    }
}
