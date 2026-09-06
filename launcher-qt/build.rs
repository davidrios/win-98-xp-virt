//! cxx-qt's cargo-only build: no CMake, no Corrosion. `CxxQtBuilder`
//! finds Qt through `qmake6`, runs `moc` and `qmltyperegistrar` over the
//! QObjects the bridges declare, compiles the generated C++ and links it
//! into this binary — so `cargo build` is still the whole build command,
//! which is what keeps this comparable to the egui launcher.
//!
//! The QML files are compiled into the binary as a Qt resource, hence
//! the `qrc:/qt/qml/<uri as a path>/…` URL `main.rs` loads: an installed
//! launcher has no `qml/` directory beside it.

use cxx_qt_build::{CxxQtBuilder, QmlModule};

fn main() {
    CxxQtBuilder::new_qml_module(
        // The same reverse-DNS identity as the rest of the product
        // (ADR-011): `com._2ksbox.…`, the leading digit escaped because
        // neither a QML module URI nor a D-Bus name may start with one.
        QmlModule::new("com._2ksbox.launcher").qml_files([
            "qml/Main.qml",
            "qml/PathField.qml",
            "qml/PresetCollection.qml",
            "qml/WizardWindow.qml",
            "qml/DiscShelfWindow.qml",
            "qml/SnapshotsWindow.qml",
            "qml/ShaderProfilesWindow.qml",
            "qml/ShaderEditorWindow.qml",
        ]),
    )
    .files([
        "src/qt/diag.rs",
        "src/qt/discs.rs",
        "src/qt/machines.rs",
        "src/qt/shaders.rs",
        "src/qt/snaps.rs",
        "src/qt/wizard.rs",
    ])
    .build();
}
