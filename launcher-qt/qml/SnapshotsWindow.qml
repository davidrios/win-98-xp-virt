// Snapshots (doc 07). A running machine goes through its monitor, a
// stopped one through `qemu-img`; the model decides which, this only
// draws it — `launcher/src/snapshots.rs`'s `SnapshotWindow::show`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

// A real top-level window — see `WizardWindow.qml`.
Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property SnapshotModel snapshots

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: body

    title: snapshots.title
    width: 740
    height: 500
    minimumWidth: 560
    minimumHeight: 320
    flags: Qt.Dialog
    color: palette.window

    /// The row whose "Restore" is armed and waiting for its confirming
    /// click. Restoring overwrites the disk's current state with the
    /// snapshot's and there is no undo, so a stray click on a row must
    /// not do it. Kept in the view because it is a property of *this
    /// window's* interaction, not of the machine.
    property string confirmRestore: ""

    // Only while a live job is in flight. The egui build polls at most
    // twice a second from inside its repaint; here the interval is
    // explicit and nothing runs when there is no job.
    Timer {
        interval: 400
        repeat: true
        running: root.visible && root.snapshots.busy
        onTriggered: root.snapshots.poll()
    }

    Rectangle {
        id: body
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: bodyLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            Label {
                Layout.fillWidth: true
                visible: root.snapshots.running
                wrapMode: Text.Wrap
                opacity: 0.75
                text: qsTr("Live: this machine is running, so a snapshot also stores its RAM and CPU state.")
            }

            Frame {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 0
                // See `Main.qml`: the style's Frame paints a border only.
                background: Rectangle {
                    color: palette.base
                    border.color: palette.mid
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 30
                        color: palette.alternateBase
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10
                            Label { text: qsTr("Name"); font.bold: true; Layout.preferredWidth: 180 }
                            Label { text: qsTr("Taken"); font.bold: true; Layout.preferredWidth: 160 }
                            Label { text: qsTr("VM state"); font.bold: true; Layout.preferredWidth: 90 }
                            Item { Layout.fillWidth: true }
                        }
                    }

                    ListView {
                        id: list
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.snapshots
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: snapRow

                            required property int index
                            required property string name
                            required property string taken
                            required property string vmState

                            width: list.width
                            implicitHeight: 40
                            color: index % 2 ? palette.base : palette.alternateBase

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                Label { text: snapRow.name; elide: Text.ElideRight; Layout.preferredWidth: 180 }
                                Label { text: snapRow.taken; Layout.preferredWidth: 160; opacity: 0.75 }
                                Label { text: snapRow.vmState; Layout.preferredWidth: 90; opacity: 0.75 }
                                Item { Layout.fillWidth: true }

                                Button {
                                    // A job in flight owns the guest's state;
                                    // a second one on top of it is refused by
                                    // QEMU anyway.
                                    enabled: !root.snapshots.busy
                                    text: root.confirmRestore === snapRow.name
                                        ? qsTr("Discard current state?")
                                        : qsTr("Restore")
                                    onClicked: {
                                        if (root.confirmRestore === snapRow.name) {
                                            root.confirmRestore = ""
                                            root.snapshots.revert(snapRow.name)
                                        } else {
                                            root.confirmRestore = snapRow.name
                                        }
                                    }
                                }
                                Button {
                                    text: qsTr("Delete")
                                    enabled: !root.snapshots.busy
                                    onClicked: {
                                        root.confirmRestore = ""
                                        root.snapshots.dropSnapshot(snapRow.name)
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: root.snapshots.count === 0
                            opacity: 0.7
                            text: qsTr("No snapshots.")
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("New snapshot") }
                TextField {
                    id: newName
                    Layout.fillWidth: true
                    selectByMouse: true
                    onAccepted: if (takeButton.enabled) takeButton.clicked()
                }
                Button {
                    id: takeButton
                    text: qsTr("Take snapshot")
                    enabled: newName.text.trim() !== "" && !root.snapshots.busy
                    onClicked: {
                        root.snapshots.take(newName.text)
                        newName.clear()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                BusyIndicator {
                    running: root.snapshots.busy
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.snapshots.status !== ""
                    text: root.snapshots.status
                    opacity: 0.75
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.snapshots.error !== ""
                text: root.snapshots.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }
        }
    }
}
