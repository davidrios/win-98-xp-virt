//! The companion launcher (doc 07), built on Qt 6 / QML through cxx-qt
//! instead of egui — a port kept beside `launcher/` so the two can be
//! compared on the same features rather than argued about in the
//! abstract. `docs/07-launcher.md`'s "Qt port" section has the findings.
//!
//! The split this port is built around: **the launcher's logic is not
//! egui's**. Nine of the sixteen `launcher/src/*.rs` modules touch no
//! toolkit type at all, and this binary re-includes those *verbatim* —
//! `#[path]` module declarations, not copies, so they cannot drift and
//! so "does it still build under another toolkit" is answered by the
//! compiler rather than by inspection. What had to be rewritten is
//! exactly the seven that draw: the window bodies, which here become
//! QObjects in `src/qt/` plus QML in `qml/`.
//!
//! The one module in between is `filepicker.rs`: the egui build needs
//! `rfd` to reach an OS file dialog because egui has none, and Qt ships
//! its own (`QtQuick.Dialogs`' `FileDialog`, which is the XDG portal on
//! Linux just like `rfd`'s backend). So this crate keeps only the
//! `Filter` type the shared modules refer to and lets QML do the
//! browsing — one dependency less.

// --- The shared half: `launcher/src/`, unchanged. -------------------
//
// Declared here at the crate root rather than under a `core::` parent so
// their own `crate::bundle` / `crate::paths` paths keep resolving with no
// edits. If any of these ever stops compiling here, it has grown a
// toolkit dependency — which is the thing worth knowing.
#[path = "../../launcher/src/bundle.rs"]
mod bundle;
#[path = "../../launcher/src/control.rs"]
mod control;
#[path = "../../launcher/src/disc_library.rs"]
mod disc_library;
#[path = "../../launcher/src/library.rs"]
mod library;
#[path = "../../launcher/src/paths.rs"]
mod paths;
#[path = "../../launcher/src/player.rs"]
mod player;
#[path = "../../launcher/src/shader_library.rs"]
mod shader_library;
#[path = "../../launcher/src/shader_profile.rs"]
mod shader_profile;
#[path = "../../launcher/src/shader_source.rs"]
mod shader_source;

// --- The Qt half. ---------------------------------------------------
mod filepicker;
mod preview;
mod snapshots;

mod qt {
    pub mod diag;
    pub mod discs;
    pub mod machines;
    pub mod shaders;
    pub mod snaps;
    pub mod wizard;
}

use cxx_qt_lib::{QGuiApplication, QQmlApplicationEngine, QString, QUrl};

/// Where `build.rs`'s QML module lands in the binary's resource tree:
/// `qrc:/qt/qml/` + the module URI with `.` as `/`.
const QML_MAIN: &str = "qrc:/qt/qml/com/_2ksbox/launcher/qml/Main.qml";

fn main() {
    // Debug verbs first, before a GUI exists — same convention as the
    // egui launcher's `--print-args` / `--diag-*-frame` (README).
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("--paths") => {
            // The same report `launcher --paths` prints, so the packaging
            // check in `scripts/package-linux.sh` would work unchanged.
            println!("player      {}", player::player_binary().display());
            println!("qemu-img    {}", player::qemu_img_binary().display());
            println!("pc-bios     {}", player::pc_bios_dir().display());
            println!("library     {}", library::default_dir().display());
            println!("discs       {}", disc_library::default_path().display());
            println!("profiles    {}", shader_library::default_dir().display());
            return;
        }
        Some("--preview-shader") => {
            // `launcher --preview-shader`'s twin, on this crate's own
            // headless device instead of eframe's: proves the preview
            // path without a window, and produces the PNG the two builds
            // are diffed against.
            let usage = "usage: launcher-qt --preview-shader <preset.slangp> <image> <out.png> [name=value,...]";
            let preset = std::path::PathBuf::from(args.next().expect(usage));
            let image = std::path::PathBuf::from(args.next().expect(usage));
            let out = args.next().expect(usage);
            let params = shader_profile::parse_params(&args.next().unwrap_or_default());
            let (w, h) = preview_area_env();
            let mut preview = preview::Preview::new();
            preview.update(&preset, &params, &image, w, h);
            if let Some(err) = preview.error() {
                eprintln!("[preview] {err}");
            }
            preview.dump_png(&out).expect("no frame rendered");
            return;
        }
        Some(other) if other.starts_with("--") && other != "--diag-frame" => {
            eprintln!("unknown option {other}");
            std::process::exit(2);
        }
        _ => {}
    }

    let mut app = QGuiApplication::new();
    let mut engine = QQmlApplicationEngine::new();
    if let Some(mut engine) = engine.as_mut() {
        // A QML error otherwise leaves the engine with no root object and
        // the process sitting in an event loop with no window at all —
        // which looks exactly like a hang.
        engine
            .as_mut()
            .on_object_creation_failed(|_, url| {
                eprintln!("[launcher-qt] {url} failed to load");
                std::process::exit(1);
            })
            .release();
        engine.load(&QUrl::from(QML_MAIN));
    }
    if let Some(app) = app.as_mut() {
        app.exec();
    }
}

/// `PREVIEW_AREA=<w>x<h>`, exactly as the egui build's shader-preview
/// verbs read it.
fn preview_area_env() -> (u32, u32) {
    std::env::var("PREVIEW_AREA")
        .ok()
        .and_then(|s| {
            let (w, h) = s.split_once('x')?;
            Some((w.parse().ok()?, h.parse().ok()?))
        })
        .unwrap_or((800, 600))
}

/// A `QString` from anything `Display`, which is most of what the bridges
/// below hand to QML (paths, errors, formatted labels).
pub fn qs(s: impl std::fmt::Display) -> QString {
    QString::from(&s.to_string())
}
