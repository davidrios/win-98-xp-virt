# launcher-qt — the Qt port of the launcher (a spike)

A second, feature-complete front end over the same launcher: machine
grid, guided creation wizard, disc shelf, snapshots, and the shader
profile manager with its live preview — on **Qt 6 / QML through
[cxx-qt](https://github.com/KDAB/cxx-qt)**, where `launcher/` uses egui.

It exists to answer "how would this go in Qt" with something that runs.
**The findings, the numbers and the recommendation are in
`docs/07-frontend.md`, section "The Qt port"**; the build and test loop
is in `docs/tracks/m6-launcher.md`. Read those, not this file.

`launcher/` remains the launcher. Nothing here is wired into the
workspace, the packaging or the test suite.

## Building

Needs Qt 6 development files — `qt6-base` and `qt6-declarative` — and
nothing else beyond the usual toolchain. There is no CMake step:
`cxx-qt-build` finds Qt through `qmake6` and drives `moc` and
`qmltyperegistrar` itself.

```sh
cd launcher-qt        # its own workspace, deliberately: see Cargo.toml
cargo build
```

The root `cargo build` does **not** build this crate, which is the point
— the Mac side, CI and the Flatpak must never start needing Qt.

## Running it against something other than your real library

`create` and `adddisc` below write files. Point the launcher's usual
environment knobs at a scratch copy first:

```sh
export LAUNCHER_LIBRARY_DIR=/tmp/lib
export LAUNCHER_DISC_LIBRARY=/tmp/discs.toml
export LAUNCHER_SHADER_PROFILES_DIR=/tmp/profiles
./target/debug/launcher-qt
```

## Debug verbs and headless screenshots

```sh
# the same report `launcher --paths` prints
./target/debug/launcher-qt --paths

# must be byte-identical to the egui build's output for the same input
./target/debug/launcher-qt --preview-shader <preset.slangp> <image> <out.png>
```

Screenshots need no clicking: `LAUNCHER_QT_SHOT=<file.png>` arms a grab,
`LAUNCHER_QT_SCREEN=` picks what to open first — `wizard`, `discs`,
`snapshots`, `profiles`, `editor`, or the scripted `create` / `adddisc`
— `LAUNCHER_QT_ARG=` is that screen's argument, and
`LAUNCHER_QT_DELAY=<ms>` is the settle time.

```sh
LAUNCHER_QT_SHOT=/tmp/editor.png LAUNCHER_QT_SCREEN=editor \
LAUNCHER_QT_ARG="/path/crt-aperture.slangp;/path/frame.png" \
  ./target/debug/launcher-qt
```

`QT_QPA_PLATFORM=offscreen` renders with no display, but `grabToImage`
needs a real session to hand back a picture — take the shots against a
running X/Wayland session.
