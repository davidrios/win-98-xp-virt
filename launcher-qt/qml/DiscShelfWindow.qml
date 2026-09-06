// The disc shelf (doc 07). Opened from the bottom button row it manages
// the shared shelf itself; opened from a machine's row it also carries
// that machine's boot-disc choice and, while it runs, live insert/eject.
//
// Library edits save as they're made — there is no "Save" button,
// because a shelf is a list of things you own, not a document being
// drafted (`launcher/src/discshelf.rs`).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import com._2ksbox.launcher

// A real top-level window — see `WizardWindow.qml`. Non-modal on
// purpose here: swapping a disc into a running machine is something you
// want to do *while* looking at it.
Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property DiscModel discs

    /// Something changed that the grid should know about.
    signal changed()

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: body

    title: discs.title
    width: 880
    height: 600
    minimumWidth: 620
    minimumHeight: 360
    flags: Qt.Dialog
    color: palette.window

    onVisibleChanged: if (!visible) root.changed()

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
                visible: !root.discs.forMachine
                wrapMode: Text.Wrap
                opacity: 0.75
                text: qsTr("Discs available to every machine. A machine picks one to boot with; the rest are swapped in while it runs.")
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.discs.forMachine
                spacing: 8
                Label { text: qsTr("Boots with: %1").arg(root.discs.bootLabel) }
                Button {
                    text: qsTr("Boot with an empty tray")
                    enabled: root.discs.hasBoot
                    onClicked: { root.discs.clearBoot(); root.changed() }
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.discs.running
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.75
                    text: qsTr("Running: “Insert” swaps the disc in the guest now; the boot choice applies next time.")
                }
                Button {
                    text: qsTr("Eject")
                    onClicked: root.discs.ejectLive()
                }
            }

            MenuSeparator { Layout.fillWidth: true }

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
                    id: shelf
                    anchors.fill: parent
                    clip: true
                    model: root.discs
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: discRow

                        required property int index
                        required property string label
                        required property string name
                        required property string dir
                        required property string path
                        required property bool isBoot

                        width: shelf.width
                        implicitHeight: 44
                        color: index % 2 ? palette.base : palette.alternateBase

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            // Buttons come *before* the path: a disc image's
                            // path is routinely long enough to widen a column
                            // past the screen, and anything after it would go
                            // with it.
                            Button {
                                text: qsTr("Insert")
                                visible: root.discs.running
                                onClicked: root.discs.insertLive(discRow.index)
                            }
                            Button {
                                text: qsTr("Boot")
                                visible: root.discs.forMachine
                                checkable: true
                                checked: discRow.isBoot
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Put this disc in the drive when the machine starts")
                                onClicked: { root.discs.setBoot(discRow.index); root.changed() }
                            }
                            Button {
                                text: qsTr("Remove")
                                onClicked: root.discs.remove(discRow.index)
                            }

                            // Editable label, committed when editing
                            // finishes rather than on every keystroke.
                            TextField {
                                Layout.preferredWidth: 190
                                text: discRow.label
                                selectByMouse: true
                                onEditingFinished: root.discs.setLabel(discRow.index, text)
                            }
                            Label {
                                Layout.preferredWidth: 170
                                text: discRow.name
                                elide: Text.ElideRight
                            }
                            Label {
                                // The directory alone, elided: truncating a
                                // full path leaves every row reading
                                // `/home/…/…/…` identically. Whole path on
                                // hover.
                                Layout.fillWidth: true
                                text: discRow.dir
                                elide: Text.ElideLeft
                                opacity: 0.6
                                ToolTip.visible: discHover.hovered
                                ToolTip.text: discRow.path
                                HoverHandler { id: discHover }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.discs.count === 0
                        opacity: 0.7
                        text: qsTr("The shelf is empty.")
                    }
                }
            }

            PathField {
                id: adder
                Layout.fillWidth: true
                label: qsTr("Add disc")
                nameFilter: root.discs.discFilter
            }

            RowLayout {
                spacing: 8
                Button {
                    text: qsTr("Add to shelf")
                    enabled: adder.value.trim() !== ""
                    onClicked: { root.discs.add(adder.value); adder.value = "" }
                }
                Button {
                    // A folder is a disc as well (M5g): `isodir` generates
                    // a volume over it as the guest reads it, which is how
                    // a pile of files reaches a machine whose networking
                    // nobody wants to trust. No name filter can express "a
                    // folder", so it is its own dialog — Qt's FolderDialog,
                    // the same portal/NSOpenPanel/IFileDialog backends
                    // PathField's FileDialog reaches.
                    text: qsTr("Add folder…")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("share a host directory with the guest as a generated disc")
                    onClicked: folderDialog.open()
                }
                Button {
                    // Doc 07's one-click guest-tools attach: no path to find,
                    // no browsing — the driver/test ISO this checkout last
                    // built.
                    text: qsTr("Add guest-tools ISO")
                    enabled: root.discs.guestToolsIso !== ""
                    ToolTip.visible: hovered
                    ToolTip.text: root.discs.guestToolsIso !== ""
                        ? root.discs.guestToolsIso
                        : qsTr("none built (guest-tools/build-wrappers.sh)")
                    onClicked: root.discs.addGuestTools()
                }
                Item { Layout.fillWidth: true }
            }

            FolderDialog {
                id: folderDialog
                title: qsTr("Share a folder with the guest")
                currentFolder: adder.value !== "" ? "file://" + adder.value : ""
                onAccepted: root.discs.add(selectedFolder.toString().replace(/^file:\/\//, ""))
            }

            Label {
                Layout.fillWidth: true
                visible: root.discs.status !== ""
                text: root.discs.status
                opacity: 0.75
            }
            Label {
                Layout.fillWidth: true
                visible: root.discs.error !== ""
                text: root.discs.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }
        }
    }
}
