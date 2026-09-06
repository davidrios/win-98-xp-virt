// The launcher window: the machine library grid, and the four windows
// off it. `launcher/src/main.rs`'s `LauncherApp::ui`, as a view.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

ApplicationWindow {
    id: root

    width: 900
    height: 560
    visible: true
    title: qsTr("2ksbox")

    // --- state ------------------------------------------------------

    MachineModel {
        id: machines
        Component.onCompleted: refresh()
    }

    // A child process has no way to push the news that it exited, so
    // this polls for it — where the egui build did the same work at the
    // top of every frame, sixty times a second, because it had a frame
    // anyway. Here the interval is stated out loud.
    Timer {
        interval: 500
        running: true
        repeat: true
        onTriggered: machines.poll()
    }

    Diag { id: diag }

    // --- the grid ---------------------------------------------------

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: qsTr("Machines")
                font.pixelSize: 18
                font.bold: true
                Layout.fillWidth: true
            }
            Label {
                text: machines.status
                opacity: 0.7
                elide: Text.ElideRight
                Layout.maximumWidth: 320
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // Column widths shared by the header and every row, so the two
        // cannot drift the way two separate layouts would.
        QtObject {
            id: cols
            readonly property int name: 190
            readonly property int family: 80
            readonly property int shader: 170
            readonly property int actions: 300
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0
            // The Basic style's Frame paints only a border, so the area
            // below the last row would otherwise show whatever is behind
            // the window — black in a grab, the window colour on screen.
            background: Rectangle {
                color: palette.base
                border.color: palette.mid
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Header row
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    color: palette.alternateBase

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Label { text: qsTr("Name"); font.bold: true; Layout.preferredWidth: cols.name }
                        Label { text: qsTr("Family"); font.bold: true; Layout.preferredWidth: cols.family }
                        Label { text: qsTr("Shader"); font.bold: true; Layout.preferredWidth: cols.shader }
                        Label { text: qsTr("Location"); font.bold: true; Layout.fillWidth: true }
                        Item { Layout.preferredWidth: cols.actions }
                    }
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: machines
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: machineRow

                        // Declared `required`, so the roles arrive as
                        // real properties of this item rather than out of
                        // a context object nothing can see.
                        required property int index
                        required property string name
                        required property string family
                        required property string shader
                        required property string location
                        required property bool running

                        width: list.width
                        implicitHeight: 40
                        color: index % 2 ? palette.base : palette.alternateBase

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Label {
                                text: machineRow.name
                                elide: Text.ElideRight
                                Layout.preferredWidth: cols.name
                            }
                            Label {
                                text: machineRow.family
                                Layout.preferredWidth: cols.family
                            }
                            Label {
                                text: machineRow.shader
                                elide: Text.ElideRight
                                Layout.preferredWidth: cols.shader
                            }
                            Label {
                                // Elides from the left: a library's paths
                                // share a long prefix, so the tail is the
                                // half that identifies the row.
                                text: machineRow.location
                                elide: Text.ElideLeft
                                opacity: 0.7
                                Layout.fillWidth: true
                                ToolTip.visible: pathHover.hovered
                                ToolTip.text: machineRow.location
                                HoverHandler { id: pathHover }
                            }

                            RowLayout {
                                spacing: 6
                                Layout.preferredWidth: cols.actions

                                Label {
                                    text: qsTr("Running")
                                    visible: machineRow.running
                                    color: palette.highlight
                                    Layout.preferredWidth: 60
                                }
                                Button {
                                    text: qsTr("Play")
                                    visible: !machineRow.running
                                    Layout.preferredWidth: 60
                                    onClicked: machines.play(machineRow.index)
                                }
                                Button {
                                    text: qsTr("Edit…")
                                    onClicked: {
                                        profiles.refresh()
                                        wizard.openEdit(machines.bundlePath(machineRow.index))
                                    }
                                }
                                Button {
                                    text: qsTr("Discs…")
                                    onClicked: {
                                        discs.openFor(machines.bundlePath(machineRow.index),
                                                      machines.discLibraryPath(),
                                                      machines.isRunning(machineRow.index))
                                        discShelfWindow.show()
                                    }
                                }
                                Button {
                                    text: qsTr("Snapshots…")
                                    onClicked: {
                                        snapshots.openFor(machines.bundlePath(machineRow.index),
                                                          machines.isRunning(machineRow.index))
                                        snapshotsWindow.show()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: machines.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        opacity: 0.7
                        text: qsTr("No machines yet.\n%1").arg(machines.libraryDir)
                    }
                }
            }
        }

        RowLayout {
            spacing: 8
            Button {
                text: qsTr("New machine…")
                onClicked: {
                    wizard.openFresh()
                    profiles.refresh()
                }
            }
            Button {
                text: qsTr("Disc shelf…")
                onClicked: {
                    discs.openLibrary(machines.discLibraryPath())
                    discShelfWindow.show()
                }
            }
            Button {
                text: qsTr("Shader profiles…")
                onClicked: {
                    profiles.refresh()
                    shaderWindow.show()
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    // --- the secondary windows --------------------------------------

    Wizard {
        id: wizard
        // The form's own `open` flag drives the window in both
        // directions, so a `submit()` that succeeds (which clears it)
        // puts the window away wherever it was called from.
        onOpenChanged: open ? wizardWindow.show() : wizardWindow.close()
    }
    DiscModel { id: discs }
    SnapshotModel { id: snapshots }
    ProfileModel { id: profiles }
    ShaderEditor {
        id: editor
        // Same shape as the wizard: the editor's own `open` flag drives
        // its window both ways, so a save (which clears it) puts the
        // window away wherever it was called from.
        onOpenChanged: open ? shaderEditorWindow.show() : shaderEditorWindow.close()
    }

    WizardWindow {
        id: wizardWindow
        wizard: wizard
        profiles: profiles
        onSaved: machines.refresh()
    }

    DiscShelfWindow {
        id: discShelfWindow
        discs: discs
        onChanged: {
            machines.refresh()
            // A disc added or renamed should show up in the guest's own
            // CDSHELF listing without restarting the machine, so every
            // *running* drive gets the new shelf file — the egui build's
            // `take_saved` loop, moved out here where the running set
            // lives.
            if (discs.takeSaved())
                machines.republishShelf()
        }
    }

    SnapshotsWindow {
        id: snapshotsWindow
        snapshots: snapshots
    }

    ShaderProfilesWindow {
        id: shaderWindow
        profiles: profiles
        editor: editor
        onChanged: machines.refresh()
    }

    ShaderEditorWindow {
        id: shaderEditorWindow
        editor: editor
        profilesDir: machines.profileDir()
        onChanged: {
            profiles.refresh()
            machines.refresh()
        }
    }

    // --- headless screenshots (see src/qt/diag.rs) -------------------

    /// The open secondary window's own QML-declared body, or null when
    /// the grid is what should be captured.
    function openWindowItem() {
        const windows = [wizardWindow, discShelfWindow, snapshotsWindow,
                         shaderWindow, shaderEditorWindow]
        for (const d of windows)
            if (d.visible && d.grabItem)
                return d.grabItem
        return null
    }

    Timer {
        running: diag.shotPath !== ""
        interval: diag.delayMs
        onTriggered: {
            diag.note("arming screen=" + diag.screen + " shot=" + diag.shotPath)
            switch (diag.screen) {
            case "wizard":
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                // `LAUNCHER_QT_ARG=<win98|xp|dos>` exercises the one piece
                // of form behaviour a screenshot can actually prove:
                // switching family moves the memory, processor,
                // acceleration and networking defaults with it, but only
                // while nobody has chosen them. The order is the shared
                // form's own (`bundle::Family::ALL`), which is also the
                // order `familyLabels()` hands the combo box, so the two
                // cannot get out of step.
                const families = ["win98", "xp", "dos"]
                if (families.indexOf(diag.arg) >= 0)
                    wizard.chooseFamily(families.indexOf(diag.arg))
                break
            case "create":
                // `LAUNCHER_QT_ARG=[<family>:]<name>` — the whole create
                // path, ending on the refreshed grid, so the run is only a
                // pass if the bundle really landed in the library. The
                // family is worth naming: a bundle written through this
                // window has to come out the same as one written by the
                // egui build or by `--wizard-new`, and DOS is the family
                // where that used to be false.
                wizard.openFresh()
                const spec = diag.arg.split(":")
                const named = spec.length > 1
                const family = named ? ["win98", "xp", "dos"].indexOf(spec[0]) : 1
                wizard.chooseFamily(family < 0 ? 1 : family)
                wizard.name = named ? spec[1] : diag.arg
                wizard.existingDisk = true
                wizard.diskPath = "/dev/null"
                diag.note("submit -> " + wizard.submit() + " " + wizard.savedPath())
                machines.refresh()
                break
            case "adddisc":
                // `LAUNCHER_QT_ARG=<path>` onto the shared shelf.
                discs.openLibrary(machines.discLibraryPath())
                discs.add(diag.arg)
                diag.note("shelf now " + discs.count + " discs, status: " + discs.status)
                discShelfWindow.show()
                break
            case "discs":
                if (diag.arg === "")
                    discs.openLibrary(machines.discLibraryPath())
                else
                    discs.openFor(diag.arg, machines.discLibraryPath(), false)
                discShelfWindow.show(); break
            case "snapshots":
                snapshots.openFor(diag.arg, false); snapshotsWindow.show(); break
            case "profiles":
                profiles.refresh(); shaderWindow.show(); break
            case "editor":
                // `<preset.slangp>;<preview image>`
                const parts = diag.arg.split(";")
                profiles.refresh()
                shaderEditorWindow.editPreset(parts[0], parts[1] || "")
                break
            }
            grabTimer.restart()
        }
    }

    // A second beat so the window just opened has been laid out and
    // rendered before the grab.
    Timer {
        id: grabTimer
        interval: diag.delayMs
        onTriggered: {
            const cb = function (result) {
                diag.report(result.saveToFile(diag.shotPath))
                Qt.quit()
            }
            // `grabToImage` only works on an item the QML engine created:
            // it starts with `qmlEngine(this)` and a window's own
            // `contentItem` (and `Overlay.overlay`) are made in C++, so
            // both refuse with no warning. Hence the grab targets below
            // are always items declared in QML — and hence a *whole
            // window* headless shot, dialog frame and all, would need a
            // small C++ shim calling `QQuickWindow::grabWindow()`.
            // Documented in doc 07: it is the one thing the egui build's
            // own 150-line off-screen dump path does better.
            const target = openWindowItem() || body
            diag.note("grabbing " + target.width + "x" + target.height
                      + " -> " + target.grabToImage(cb))
        }
    }
}
