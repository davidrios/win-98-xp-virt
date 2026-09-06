//! Headless screenshots, the way `launcher --diag-*-frame` does them for
//! the egui build.
//!
//! The egui launcher has to *simulate* a frame to get one without a
//! window: build an `egui::Context`, feed it synthetic pointer events,
//! tessellate, paint into an off-screen texture, dump the texture
//! (`main.rs`'s `dump_egui_frame` and `diag_window_frames`, ~150 lines).
//! Qt Quick already knows how to render off-screen — `QT_QPA_PLATFORM=
//! offscreen` with the software backend, and `Item.grabToImage()` — so
//! this side is a handful of environment variables read into properties
//! and four lines of QML. That is the largest single code saving in the
//! whole port, and it is entirely because Qt separates "render" from
//! "have a window" and egui does not.
//!
//! `LAUNCHER_QT_SHOT=<file.png>` arms it, `LAUNCHER_QT_SCREEN=<name>`
//! picks which window to open first, `LAUNCHER_QT_ARG=<value>` is that
//! window's argument (a bundle path, a preset), and
//! `LAUNCHER_QT_DELAY=<ms>` is how long to let it settle.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        /// Empty unless `LAUNCHER_QT_SHOT` is set, which is what QML
        /// checks to decide whether any of this is happening at all.
        #[qproperty(QString, shot_path)]
        /// "", "wizard", "discs", "snapshots", "profiles", "editor".
        #[qproperty(QString, screen)]
        #[qproperty(QString, arg)]
        #[qproperty(i32, delay_ms)]
        type Diag = super::DiagRust;

        /// Report what the grab did, so a failed `saveToFile` is visible
        /// in the terminal instead of producing a silent empty run.
        #[qinvokable]
        fn report(self: &Diag, ok: bool);

        /// A trace line from QML. Not `console.log`: that goes through
        /// Qt's categorised logging, which drops the `qml` category's
        /// debug output unless `QT_LOGGING_RULES` says otherwise — a
        /// good half hour went into noticing that. This always prints.
        #[qinvokable]
        fn note(self: &Diag, message: &QString);
    }
}

use crate::qs;
use cxx_qt_lib::QString;

pub struct DiagRust {
    shot_path: QString,
    screen: QString,
    arg: QString,
    delay_ms: i32,
}

fn env(name: &str) -> QString {
    std::env::var(name).map(|v| qs(v)).unwrap_or_default()
}

impl Default for DiagRust {
    fn default() -> Self {
        DiagRust {
            shot_path: env("LAUNCHER_QT_SHOT"),
            screen: env("LAUNCHER_QT_SCREEN"),
            arg: env("LAUNCHER_QT_ARG"),
            delay_ms: std::env::var("LAUNCHER_QT_DELAY")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(600),
        }
    }
}

impl ffi::Diag {
    fn note(&self, message: &QString) {
        eprintln!("[diag] {message}");
    }

    fn report(&self, ok: bool) {
        if ok {
            println!("[diag] wrote {}", self.shot_path);
        } else {
            eprintln!("[diag] failed to write {}", self.shot_path);
        }
    }
}
