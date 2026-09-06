//! Shader profiles (doc 07), as two QML models over
//! `launcher_core::editor`.
//!
//! `ProfileModel` is the profile list — New / Edit / Delete over
//! `shader_library`. `ShaderEditor` wraps `editor::Editor` and is
//! *itself* the list model for the preset's parameters, which is the one
//! place this port came out structurally nicer than the egui build:
//! there, the parameter metadata and the overrides are two vectors a
//! `for` loop zips into sliders; here they are the model's rows and the
//! slider is a delegate, so "the checkbox and the slider disagree about
//! which parameter they belong to" stops being expressible.
//!
//! Everything the editor *does* is the shared model: reading a preset,
//! keeping only overridden parameters, the "only a drag counts" guard
//! that stops a preset whose default sits off the step grid from
//! silently acquiring an override just by being opened, and saving a new
//! profile *with* its overrides. The preset collection and its download
//! are `editor::Presets`.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qvariant.h");
        type QVariant = cxx_qt_lib::QVariant;
        include!("cxx-qt-lib/qmodelindex.h");
        type QModelIndex = cxx_qt_lib::QModelIndex;
        include!("cxx-qt-lib/qhash.h");
        type QHash_i32_QByteArray = cxx_qt_lib::QHash<cxx_qt_lib::QHashPair_i32_QByteArray>;
    }

    unsafe extern "C++" {
        include!(<QtCore/QAbstractListModel>);
        type QAbstractListModel;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[base = QAbstractListModel]
        #[qml_element]
        #[qproperty(i32, count)]
        type ProfileModel = super::ProfileModelRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &ProfileModel, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &ProfileModel, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &ProfileModel) -> QHash_i32_QByteArray;

        #[qinvokable]
        fn refresh(self: Pin<&mut ProfileModel>);

        #[qinvokable]
        fn path_at(self: &ProfileModel, row: i32) -> QString;

        #[qinvokable]
        fn id_at(self: &ProfileModel, row: i32) -> QString;

        /// The profile's display name. A plain invokable rather than
        /// making QML reach through `index()`/`data()` for one string.
        #[qinvokable]
        fn name_at(self: &ProfileModel, row: i32) -> QString;

        /// The row whose id matches, or -1 — for the wizard's profile
        /// combo, which has to show the machine's saved choice.
        #[qinvokable]
        fn row_of_id(self: &ProfileModel, id: &QString) -> i32;

        #[qinvokable]
        fn delete_at(self: Pin<&mut ProfileModel>, row: i32);
    }

    #[auto_cxx_name]
    extern "RustQt" {
        /// # Safety
        /// Inherited; paired with `end_reset_model`.
        #[inherit]
        unsafe fn begin_reset_model(self: Pin<&mut ProfileModel>);

        /// # Safety
        /// Inherited; pairs with `begin_reset_model`.
        #[inherit]
        unsafe fn end_reset_model(self: Pin<&mut ProfileModel>);
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[base = QAbstractListModel]
        #[qml_element]
        #[qproperty(bool, open)]
        #[qproperty(i32, count)]
        #[qproperty(QString, name)]
        #[qproperty(QString, preset_path)]
        #[qproperty(QString, preview_image)]
        #[qproperty(QString, parse_error)]
        #[qproperty(QString, error)]
        /// The URL QML's `Image` reads, carrying a generation counter so
        /// every rendered frame is a new URL — see `src/preview.rs` for
        /// why the frame goes through a file at all.
        #[qproperty(QString, preview_source)]
        /// The size the last frame came out at: the source image's own
        /// size times the largest integer scale that fits, exactly as
        /// `player::Gpu::viewport` computes it. QML centres a frame this
        /// size on black rather than stretching it to fill.
        #[qproperty(i32, preview_width)]
        #[qproperty(i32, preview_height)]
        /// How many milliseconds until the preview wants drawing again,
        /// or 0 when it never does: a preset whose picture depends on the
        /// frame number (an interlaced CRT, a phosphor afterglow, a
        /// shimmering NTSC signal) is only itself in motion, and one that
        /// does not must not spin a timer. QML runs its render timer at
        /// this; the egui build asks egui to repaint after it.
        #[qproperty(i32, preview_interval)]
        /// Where the preset collection is, or "" if there is none yet.
        #[qproperty(QString, presets_dir)]
        /// "", "running:<MB>", "failed:<message>" — the download's state.
        #[qproperty(QString, download_state)]
        /// Where the collection would be installed, for the offer.
        #[qproperty(QString, presets_install_dir)]
        #[qproperty(QString, presets_download_size)]
        /// The file dialogs' name filters, from the same constants the
        /// egui build hands `rfd`.
        #[qproperty(QString, preset_filter)]
        #[qproperty(QString, image_filter)]
        type ShaderEditor = super::ShaderEditorRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &ShaderEditor, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &ShaderEditor, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &ShaderEditor) -> QHash_i32_QByteArray;

        #[qinvokable]
        fn new_profile(self: Pin<&mut ShaderEditor>);

        #[qinvokable]
        fn edit(self: Pin<&mut ShaderEditor>, path: &QString);

        /// Re-read the preset's parameters if the path changed. Called
        /// when the preset field is committed, where the egui build does
        /// it once per frame.
        #[qinvokable]
        fn reparse(self: Pin<&mut ShaderEditor>);

        /// Override (or stop overriding) one parameter.
        #[qinvokable]
        fn set_override(self: Pin<&mut ShaderEditor>, row: i32, enabled: bool);

        /// Move an overridden parameter. Ignored for a row that isn't
        /// overridden — a disabled slider must not be able to write one.
        #[qinvokable]
        fn set_value(self: Pin<&mut ShaderEditor>, row: i32, value: f32);

        /// Render one preview frame into `area` pixels.
        #[qinvokable]
        fn render(self: Pin<&mut ShaderEditor>, area_w: i32, area_h: i32);

        /// Save the profile. Returns true once written.
        #[qinvokable]
        fn save(self: Pin<&mut ShaderEditor>, profiles_dir: &QString) -> bool;

        #[qinvokable]
        fn download_presets(self: Pin<&mut ShaderEditor>);

        /// Poll a running download. Driven by a QML `Timer`.
        #[qinvokable]
        fn poll_download(self: Pin<&mut ShaderEditor>);
    }

    #[auto_cxx_name]
    extern "RustQt" {
        /// # Safety
        /// Inherited; paired with `end_reset_model`.
        #[inherit]
        unsafe fn begin_reset_model(self: Pin<&mut ShaderEditor>);

        /// # Safety
        /// Inherited; pairs with `begin_reset_model`.
        #[inherit]
        unsafe fn end_reset_model(self: Pin<&mut ShaderEditor>);
    }
}

use crate::{preview, qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use launcher_core::browse::name_filter;
use launcher_core::editor::{Editor, PresetState, Presets, IMAGE_FILTER, PRESET_FILTER};
use launcher_core::shader_library;
use std::path::PathBuf;
use std::pin::Pin;

// --- the profile list ------------------------------------------------

const P_NAME: i32 = 0;
const P_PRESET: i32 = 1;
const P_ID: i32 = 2;

#[derive(Default)]
pub struct ProfileModelRust {
    count: i32,
    dir: PathBuf,
    entries: Vec<shader_library::ProfileEntry>,
}

impl ffi::ProfileModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.entries.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(entry) = self.entries.get(index.row() as usize) else {
            return QVariant::default();
        };
        match role {
            P_NAME => QVariant::from(&qs(&entry.profile.name)),
            P_PRESET => QVariant::from(&qs(entry.profile.preset.display())),
            P_ID => QVariant::from(&qs(shader_library::id_of(&entry.path))),
            _ => QVariant::default(),
        }
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(P_NAME, QByteArray::from("name"));
        roles.insert(P_PRESET, QByteArray::from("preset"));
        roles.insert(P_ID, QByteArray::from("profileId"));
        roles
    }

    fn refresh(mut self: Pin<&mut Self>) {
        unsafe { self.as_mut().begin_reset_model() };
        {
            let mut this = self.as_mut().rust_mut();
            if this.dir.as_os_str().is_empty() {
                this.dir = shader_library::default_dir();
            }
            this.entries = shader_library::scan(&this.dir);
        }
        unsafe { self.as_mut().end_reset_model() };
        let count = self.entries.len() as i32;
        self.as_mut().set_count(count);
    }

    fn path_at(&self, row: i32) -> QString {
        self.entries.get(row as usize).map(|e| qs(e.path.display())).unwrap_or_default()
    }

    fn id_at(&self, row: i32) -> QString {
        self.entries.get(row as usize).map(|e| qs(shader_library::id_of(&e.path))).unwrap_or_default()
    }

    fn name_at(&self, row: i32) -> QString {
        self.entries.get(row as usize).map(|e| qs(&e.profile.name)).unwrap_or_default()
    }

    fn row_of_id(&self, id: &QString) -> i32 {
        let id = id.to_string();
        self.entries
            .iter()
            .position(|e| shader_library::id_of(&e.path) == id)
            .map(|i| i as i32)
            .unwrap_or(-1)
    }

    fn delete_at(self: Pin<&mut Self>, row: i32) {
        let Some(path) = self.entries.get(row as usize).map(|e| e.path.clone()) else { return };
        if let Err(e) = shader_library::delete(&path) {
            eprintln!("[shader-manager] deleting {}: {e}", path.display());
        }
        self.refresh();
    }
}

// --- the editor, which is also the parameter list ---------------------

const E_ID: i32 = 0;
const E_DESCRIPTION: i32 = 1;
const E_MINIMUM: i32 = 2;
const E_MAXIMUM: i32 = 3;
const E_STEP: i32 = 4;
const E_DEFAULT: i32 = 5;
const E_VALUE: i32 = 6;
const E_OVERRIDDEN: i32 = 7;

#[derive(Default)]
pub struct ShaderEditorRust {
    open: bool,
    count: i32,
    name: QString,
    preset_path: QString,
    preview_image: QString,
    parse_error: QString,
    error: QString,
    preview_source: QString,
    preview_width: i32,
    preview_height: i32,
    preview_interval: i32,
    presets_dir: QString,
    download_state: QString,
    presets_install_dir: QString,
    presets_download_size: QString,
    preset_filter: QString,
    image_filter: QString,

    /// The editor. Everything above is a projection of it (plus the
    /// preview's own output).
    model: Editor,
    presets: Presets,
    preview: Option<preview::Preview>,
}

impl ffi::ShaderEditor {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.rust().model.params().len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let editor = &self.rust().model;
        let row = index.row() as usize;
        let Some((meta, over)) = editor.param(row) else {
            return QVariant::default();
        };
        match role {
            E_ID => QVariant::from(&qs(&meta.id)),
            // The editor hides a description that just repeats the id;
            // same rule for both front ends, decided there.
            E_DESCRIPTION => QVariant::from(&qs_opt(editor.description(row))),
            E_MINIMUM => QVariant::from(&meta.minimum),
            E_MAXIMUM => QVariant::from(&meta.maximum),
            E_STEP => QVariant::from(&meta.step),
            E_DEFAULT => QVariant::from(&meta.default),
            E_VALUE => QVariant::from(&over.unwrap_or(meta.default)),
            E_OVERRIDDEN => QVariant::from(&over.is_some()),
            _ => QVariant::default(),
        }
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(E_ID, QByteArray::from("paramId"));
        roles.insert(E_DESCRIPTION, QByteArray::from("description"));
        roles.insert(E_MINIMUM, QByteArray::from("minimum"));
        roles.insert(E_MAXIMUM, QByteArray::from("maximum"));
        roles.insert(E_STEP, QByteArray::from("step"));
        roles.insert(E_DEFAULT, QByteArray::from("defaultValue"));
        roles.insert(E_VALUE, QByteArray::from("value"));
        roles.insert(E_OVERRIDDEN, QByteArray::from("overridden"));
        roles
    }

    fn new_profile(mut self: Pin<&mut Self>) {
        self.as_mut().with_rows(|e| e.new_profile());
        self.as_mut().rust_mut().preview = None;
        self.publish();
    }

    fn edit(mut self: Pin<&mut Self>, path: &QString) {
        let path = PathBuf::from(path.to_string());
        self.as_mut().with_rows(|e| e.edit_path(path));
        self.as_mut().rust_mut().preview = None;
        self.publish();
    }

    fn reparse(mut self: Pin<&mut Self>) {
        // The property is where QML's committed text is; the model needs
        // it before it can decide whether anything changed.
        let preset = self.preset_path.to_string();
        let image = self.preview_image.to_string();
        self.as_mut().with_rows(|e| {
            e.preset_path = preset;
            e.preview_image_path = image;
            e.reparse();
        });
        self.publish();
    }

    fn set_override(mut self: Pin<&mut Self>, row: i32, enabled: bool) {
        if row < 0 {
            return;
        }
        self.as_mut().with_rows(|e| e.set_override(row as usize, enabled));
    }

    fn set_value(mut self: Pin<&mut Self>, row: i32, value: f32) {
        if row < 0 {
            return;
        }
        self.as_mut().rust_mut().model.set_value(row as usize, value);
    }

    fn render(mut self: Pin<&mut Self>, area_w: i32, area_h: i32) {
        {
            // The two path fields are two-way-bound properties; catch the
            // model up before asking whether it can render.
            let (preset, image) = (self.preset_path.to_string(), self.preview_image.to_string());
            let mut this = self.as_mut().rust_mut();
            this.model.preset_path = preset;
            this.model.preview_image_path = image;
        }
        if !self.rust().model.renderable() {
            self.as_mut().set_preview_interval(0); // nothing to animate
            return;
        }
        let (source, w, h, err, interval) = {
            let mut this = self.as_mut().rust_mut();
            let params = this.model.effective();
            let preset = PathBuf::from(this.model.preset_path.trim());
            let image = PathBuf::from(this.model.preview_image_path.trim());
            let preview = this.preview.get_or_insert_with(preview::Preview::new);
            preview.update(&preset, &params, &image, area_w.max(1) as u32, area_h.max(1) as u32);
            let (w, h) = preview.viewport();
            let interval = preview.frame_interval_ms();
            (qs(preview.source_url()), w as i32, h as i32, qs_opt(preview.error()), interval)
        };
        self.as_mut().set_preview_source(source);
        self.as_mut().set_preview_width(w);
        self.as_mut().set_preview_height(h);
        self.as_mut().set_preview_interval(interval);
        self.as_mut().set_error(err);
    }

    fn save(mut self: Pin<&mut Self>, profiles_dir: &QString) -> bool {
        let dir = PathBuf::from(profiles_dir.to_string());
        let (name, preset) = (self.name.to_string(), self.preset_path.to_string());
        let ok = {
            let mut this = self.as_mut().rust_mut();
            this.model.name = name;
            this.model.preset_path = preset;
            this.model.save(&dir)
        };
        self.publish();
        ok
    }

    fn download_presets(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().presets.start_download();
        self.publish();
    }

    fn poll_download(self: Pin<&mut Self>) {
        if !self.rust().presets.download_active() {
            return;
        }
        self.publish();
    }
}

impl ffi::ShaderEditor {
    /// Run an editor operation that may change the parameter rows,
    /// bracketed so attached views are told.
    fn with_rows(mut self: Pin<&mut Self>, op: impl FnOnce(&mut Editor)) {
        // Safety: paired with `end_reset_model` immediately below.
        unsafe { self.as_mut().begin_reset_model() };
        op(&mut self.as_mut().rust_mut().model);
        unsafe { self.as_mut().end_reset_model() };
        let count = self.rust().model.params().len() as i32;
        self.as_mut().set_count(count);
    }

    /// The editor and the preset collection, onto the properties — every
    /// one through its own setter (see the header of `main.rs`).
    fn publish(mut self: Pin<&mut Self>) {
        let (open, count, name, preset_path, preview_image, parse_error, error);
        let (presets_dir, download_state, install_dir, size, preset_filter, image_filter);
        {
            // `Presets::state` advances a finished download into the
            // cached directory, so it needs `&mut`.
            let this = &mut *self.as_mut().rust_mut();
            let state = this.presets.state();
            let e = &this.model;
            open = e.open;
            count = e.params().len() as i32;
            name = qs(&e.name);
            preset_path = qs(&e.preset_path);
            preview_image = qs(&e.preview_image_path);
            parse_error = qs_opt(e.parse_error());
            error = qs_opt(e.error.as_deref());
            (presets_dir, download_state, install_dir, size) = match state {
                PresetState::Ready(dir) => {
                    (qs(dir.display()), QString::default(), QString::default(), QString::default())
                }
                PresetState::Downloading(mb) => (
                    QString::default(),
                    qs(format!("running:{mb:.1}")),
                    QString::default(),
                    QString::default(),
                ),
                PresetState::Failed(e) => {
                    (QString::default(), qs(format!("failed:{e}")), QString::default(), QString::default())
                }
                PresetState::Missing { install_dir, size } => (
                    QString::default(),
                    QString::default(),
                    qs(install_dir.display()),
                    QString::from(size),
                ),
            };
            preset_filter = qs(name_filter(PRESET_FILTER));
            image_filter = qs(name_filter(IMAGE_FILTER));
        }
        self.as_mut().set_open(open);
        self.as_mut().set_count(count);
        self.as_mut().set_name(name);
        self.as_mut().set_preset_path(preset_path);
        self.as_mut().set_preview_image(preview_image);
        self.as_mut().set_parse_error(parse_error);
        self.as_mut().set_error(error);
        self.as_mut().set_presets_dir(presets_dir);
        self.as_mut().set_download_state(download_state);
        self.as_mut().set_presets_install_dir(install_dir);
        self.as_mut().set_presets_download_size(size);
        self.as_mut().set_preset_filter(preset_filter);
        self.as_mut().set_image_filter(image_filter);
    }
}
