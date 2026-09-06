// The guided creation wizard (doc 07), over the shared form
// (`launcher_core::wizard::Form`). The same form doubles as "Edit
// machine" for an existing bundle, and the egui build draws the same
// fields from the same model.
//
// Every field with a *consequence* goes through an invokable
// (`chooseFamily`, `chooseRam`, …) rather than assigning the property:
// see the header of `src/qt/wizard.rs` for why.
//
// The combo boxes' labels and the file dialogs' name filters come from
// the model too (`familyLabels()`, `diskFilter()`, …) rather than being
// retyped here, so the two front ends cannot end up offering differently
// worded choices — which they did, for the family combo, until the form
// became shared.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

// A real top-level window, not an in-window popup: the launcher's
// secondary screens are separate windows the user can move, resize and
// leave open beside the grid, which is what the platform already knows
// how to do. `Qt.Dialog` keeps it transient for the launcher window (the
// compositor stacks it above and gives it a dialog frame) without making
// it modal — nothing here needs to block the grid.
Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property Wizard wizard
    required property ProfileModel profiles

    signal saved()

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: form

    title: wizard.title
    width: 660
    height: 720
    minimumWidth: 520
    minimumHeight: 420
    flags: Qt.Dialog
    color: palette.window

    // Closing the window *is* cancelling the form: the flag drives the
    // window in both directions (`Main.qml`), so clearing it here keeps
    // the two from disagreeing after a close from the title bar.
    onVisibleChanged: if (!visible && wizard.open) wizard.open = false

    Rectangle {
        id: form
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: formLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            GridLayout {
                columns: 2
                columnSpacing: 8
                rowSpacing: 8
                Layout.fillWidth: true

                Label { text: qsTr("Family") }
                ComboBox {
                    Layout.fillWidth: true
                    model: root.wizard.familyLabels()
                    currentIndex: root.wizard.family
                    onActivated: root.wizard.chooseFamily(currentIndex)
                }

                Label { text: qsTr("Name") }
                TextField {
                    Layout.fillWidth: true
                    text: root.wizard.name
                    selectByMouse: true
                    onTextChanged: root.wizard.name = text
                }
            }

            MenuSeparator { Layout.fillWidth: true }

            // --- memory -------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Memory (MB)"); Layout.minimumWidth: 150 }
                SpinBox {
                    id: ram
                    from: root.wizard.ramMin
                    to: root.wizard.ramMax
                    stepSize: 16
                    editable: true
                    value: root.wizard.ramMb
                    onValueModified: root.wizard.chooseRam(value)
                }
                Button {
                    text: qsTr("Default")
                    enabled: !root.wizard.ramIsDefault
                    onClicked: root.wizard.resetRam()
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                visible: root.wizard.ramNote !== ""
                text: root.wizard.ramNote
                wrapMode: Text.Wrap
                font.pixelSize: 11
                opacity: 0.75
            }

            // --- processor ----------------------------------------------
            // Named machines rather than a number: "how many instructions
            // per second" is not something anyone knows about their DOS
            // game, while "it wants a 486" is written on the box. This is
            // the field that decides whether an era game is playable at
            // all (doc 06), and the Qt port had no widget for it until the
            // form became shared.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Processor"); Layout.minimumWidth: 150 }
                ComboBox {
                    Layout.preferredWidth: 200
                    model: root.wizard.cpuSpeedLabels()
                    currentIndex: root.wizard.cpuSpeed
                    onActivated: root.wizard.chooseCpuSpeed(currentIndex)
                }
                Button {
                    text: qsTr("Default")
                    enabled: !root.wizard.cpuIsDefault
                    onClicked: root.wizard.resetCpuSpeed()
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                visible: root.wizard.cpuNote !== ""
                text: root.wizard.cpuNote
                wrapMode: Text.Wrap
                font.pixelSize: 11
                opacity: 0.75
            }

            // --- acceleration -------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Acceleration"); Layout.minimumWidth: 150 }
                ComboBox {
                    Layout.preferredWidth: 200
                    model: root.wizard.accelLabels()
                    currentIndex: root.wizard.accel
                    onActivated: root.wizard.chooseAccel(currentIndex)
                }
                Button {
                    text: qsTr("Default")
                    enabled: !root.wizard.accelIsDefault
                    onClicked: root.wizard.resetAccel()
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                text: root.wizard.accelNote
                wrapMode: Text.Wrap
                font.pixelSize: 11
                // The one case that is a warning rather than a note: KVM was
                // demanded and this host hasn't got it, so the machine will
                // refuse to start.
                color: root.wizard.accelWarning ? "#c88200" : palette.windowText
                opacity: root.wizard.accelWarning ? 1.0 : 0.75
            }

            // --- networking ---------------------------------------------
            CheckBox {
                text: qsTr("Networking")
                checked: root.wizard.network
                onToggled: root.wizard.chooseNetwork(checked)
            }
            Label {
                Layout.fillWidth: true
                text: root.wizard.networkNote
                wrapMode: Text.Wrap
                font.pixelSize: 11
                opacity: 0.75
            }

            MenuSeparator { Layout.fillWidth: true }

            // --- disk ---------------------------------------------------
            CheckBox {
                visible: !root.wizard.editing
                text: qsTr("Use an existing disk image")
                checked: root.wizard.existingDisk
                onToggled: root.wizard.existingDisk = checked
            }
            PathField {
                Layout.fillWidth: true
                visible: root.wizard.editing || root.wizard.existingDisk
                label: qsTr("Disk path")
                nameFilter: root.wizard.diskFilter()
                value: root.wizard.diskPath
                onValueChanged: root.wizard.diskPath = value
            }
            RowLayout {
                Layout.fillWidth: true
                visible: !root.wizard.editing && !root.wizard.existingDisk
                spacing: 8
                Label { text: qsTr("New disk size (GB)"); Layout.minimumWidth: 150 }
                SpinBox {
                    from: 1
                    to: 128
                    value: root.wizard.diskSizeGb
                    editable: true
                    onValueModified: root.wizard.diskSizeGb = value
                }
                Item { Layout.fillWidth: true }
            }
            PathField {
                Layout.fillWidth: true
                label: qsTr("Install media (optional)")
                nameFilter: root.wizard.mediaFilter()
                value: root.wizard.installMedia
                onValueChanged: root.wizard.installMedia = value
            }
            // A floppy in A:, and what the machine boots from — doc 06
            // lists a floppy on the Win98 machine and doc 07 lists floppy
            // images among the media the launcher handles. Two more
            // fields this port did not have.
            PathField {
                Layout.fillWidth: true
                label: qsTr("Floppy (optional)")
                nameFilter: root.wizard.floppyFilter()
                value: root.wizard.floppy
                // Through an invokable, not the property: the boot note
                // below depends on whether there is an image at all.
                onValueChanged: root.wizard.setFloppyPath(value)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Boot from"); Layout.minimumWidth: 150 }
                ComboBox {
                    Layout.preferredWidth: 200
                    model: root.wizard.bootLabels()
                    currentIndex: root.wizard.boot
                    onActivated: root.wizard.chooseBoot(currentIndex)
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                visible: root.wizard.bootNote !== ""
                text: root.wizard.bootNote
                wrapMode: Text.Wrap
                font.pixelSize: 11
                opacity: 0.75
            }

            MenuSeparator { Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Shader profile"); Layout.minimumWidth: 150 }
                ComboBox {
                    id: profileBox
                    Layout.fillWidth: true
                    // Index 0 is "(default)"; the profiles follow it, so a
                    // row in `profiles` is at index+1 here.
                    model: root.profiles.count + 1
                    displayText: currentIndex === 0
                        ? qsTr("(default)")
                        : root.profiles.nameAt(currentIndex - 1)
                    currentIndex: {
                        const row = root.profiles.rowOfId(root.wizard.shaderProfile)
                        return row < 0 ? 0 : row + 1
                    }
                    delegate: ItemDelegate {
                        required property int index
                        width: profileBox.width
                        text: index === 0
                            ? qsTr("(default)")
                            : root.profiles.nameAt(index - 1)
                        onClicked: {
                            profileBox.currentIndex = index
                            root.wizard.shaderProfile =
                                index === 0 ? "" : root.profiles.idAt(index - 1)
                            profileBox.popup.close()
                        }
                    }
                }
            }

            MenuSeparator { Layout.fillWidth: true }

            CheckBox {
                text: qsTr("Advanced: edit machine.toml directly")
                checked: root.wizard.advanced
                onToggled: {
                    root.wizard.advanced = checked
                    if (checked)
                        root.wizard.fillAdvanced()
                }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                visible: root.wizard.advanced
                TextArea {
                    text: root.wizard.advancedToml
                    font.family: "monospace"
                    selectByMouse: true
                    onTextChanged: root.wizard.advancedToml = text
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.wizard.error !== ""
                text: root.wizard.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            // Pushes the buttons to the bottom edge however tall the
            // window is dragged.
            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.wizard.open = false
                }
                Button {
                    text: root.wizard.editing ? qsTr("Save") : qsTr("Create")
                    // A failed submit leaves the window open with the
                    // reason in the error line above.
                    onClicked: if (root.wizard.submit()) root.saved()
                }
            }
        }
    }
}
