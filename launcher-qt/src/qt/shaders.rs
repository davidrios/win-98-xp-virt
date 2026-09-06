//! Shader profiles (doc 07), as two QML models.
//!
//! `ProfileModel` is the profile list — New / Edit / Delete over
//! `shader_library`. `ShaderEditor` is the editor, and it is *itself* the
//! list model for the preset's parameters, which is the one place this
//! port came out structurally nicer than the egui build: there, an
//! `Editor` holds `params: Vec<ParamMeta>` and `overrides:
//! Vec<Option<f32>>` in lockstep and a `for` loop zips them into
//! sliders; here the two vectors are the model's rows and the slider is
//! a delegate, so "the checkbox and the slider disagree about which
//! parameter they belong to" stops being expressible.
//!
//! Everything else is the same code: `shader_profile::parameter_meta` to
//! read a preset, only-overridden parameters saved, the un-overridden
//! ones left at the preset's own default (so a profile survives the
//! preset gaining parameters later), and the "only a drag counts" guard
//! that stops a preset whose default sits off the step grid from
//! silently acquiring an override just by being opened.

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
        /// Where the preset collection is, or "" if there is none yet.
        #[qproperty(QString, presets_dir)]
        /// "", "running:<MB>", "failed:<message>" — the download's state.
        #[qproperty(QString, download_state)]
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

use crate::shader_profile::{self, ParamMeta, ShaderProfile};
use crate::shader_source::{self, Download, Status};
use crate::{preview, qs, shader_library};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use std::path::{Path, PathBuf};
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

    fn delete_at(mut self: Pin<&mut Self>, row: i32) {
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
    presets_dir: QString,
    download_state: QString,

    /// `None` for a new profile; `Some(path)` to save back in place.
    path: Option<PathBuf>,
    /// The last preset successfully parsed, so parameters are only
    /// re-read when the path actually changes.
    parsed_preset: Option<PathBuf>,
    params: Vec<ParamMeta>,
    /// One entry per `params`, same order: `Some(value)` when overridden.
    overrides: Vec<Option<f32>>,
    preview: Option<preview::Preview>,
    download: Option<Download>,
    /// The preset collection is *cached*: finding it walks the
    /// collection's top two levels, which is nothing once but not
    /// something to repeat per frame.
    looked_for_presets: bool,
}


impl ShaderEditorRust {
    /// Re-read the preset's parameters if the path changed, and return
    /// the parse error to publish (empty when it parsed). Deliberately
    /// *returns* rather than assigning `self.parse_error`: that field is
    /// a Q_PROPERTY, and writing it here would change what QML reads
    /// without emitting its notify — see the header of `wizard.rs`.
    fn reparse(&mut self, preset_path: &str) -> QString {
        let trimmed = preset_path.trim();
        if trimmed.is_empty() {
            self.params.clear();
            self.overrides.clear();
            self.parsed_preset = None;
            return QString::default();
        }
        let path = Path::new(trimmed);
        if self.parsed_preset.as_deref() == Some(path) {
            return self.parse_error.clone();
        }
        self.parsed_preset = Some(path.to_path_buf());
        match shader_profile::parameter_meta(path) {
            Ok(params) => {
                self.overrides = vec![None; params.len()];
                self.params = params;
                QString::default()
            }
            Err(e) => {
                self.params.clear();
                self.overrides.clear();
                qs(e)
            }
        }
    }

    fn build(&self, name: String, preset: PathBuf) -> ShaderProfile {
        let mut profile = ShaderProfile::new(name, preset);
        for (meta, over) in self.params.iter().zip(self.overrides.iter()) {
            if let Some(v) = over {
                profile.params.insert(meta.id.clone(), *v);
            }
        }
        profile
    }

    /// The values the preview actually renders with: the override where
    /// there is one, the preset's own default everywhere else.
    fn effective(&self) -> Vec<(String, f32)> {
        self.params
            .iter()
            .zip(&self.overrides)
            .map(|(meta, over)| (meta.id.clone(), over.unwrap_or(meta.default)))
            .collect()
    }

    /// Where the preset collection is, cached: finding it walks the
    /// collection's top two levels, which is nothing once but not
    /// something to repeat per frame. Returns the value to publish.
    fn find_presets(&mut self) -> QString {
        if self.looked_for_presets {
            return self.presets_dir.clone();
        }
        self.looked_for_presets = true;
        shader_source::presets_dir().map(|d| qs(d.display())).unwrap_or_default()
    }

    /// Forget the parsed preset, the sliders and the chain. The
    /// Q_PROPERTY fields are *not* touched here; `reset` in the QObject
    /// below clears those through their setters.
    fn clear(&mut self) {
        self.path = None;
        self.parsed_preset = None;
        self.params.clear();
        self.overrides.clear();
        self.preview = None;
        self.download = None;
    }
}

impl ffi::ShaderEditor {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.params.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let row = index.row() as usize;
        let (Some(meta), Some(over)) = (self.params.get(row), self.overrides.get(row)) else {
            return QVariant::default();
        };
        match role {
            E_ID => QVariant::from(&qs(&meta.id)),
            E_DESCRIPTION => QVariant::from(&qs(
                // The egui build hides a description that just repeats
                // the id; same rule, decided here so QML needn't.
                if meta.description.is_empty() || meta.description == meta.id {
                    String::new()
                } else {
                    meta.description.clone()
                },
            )),
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
        self.as_mut().reset();
        let presets = self.as_mut().rust_mut().find_presets();
        self.as_mut().set_presets_dir(presets);
        self.as_mut().set_open(true);
    }

    fn edit(mut self: Pin<&mut Self>, path: &QString) {
        let path = PathBuf::from(path.to_string());
        let profile = ShaderProfile::load(&path);
        self.as_mut().reset();
        let presets = self.as_mut().rust_mut().find_presets();
        self.as_mut().set_presets_dir(presets);
        match profile {
            Ok(profile) => {
                let preset = qs(profile.preset.display());
                // Safety: paired with `end_reset_model` below.
                unsafe { self.as_mut().begin_reset_model() };
                let parse_error = {
                    let mut this = self.as_mut().rust_mut();
                    this.path = Some(path);
                    let parse_error = this.reparse(&preset.to_string());
                    // Line the overrides up with the freshly parsed
                    // parameters *after* reparse, so a preset that
                    // dropped one since the profile was saved doesn't
                    // leave a dangling override.
                    for i in 0..this.params.len() {
                        if let Some(&v) = profile.params.get(&this.params[i].id) {
                            this.overrides[i] = Some(v);
                        }
                    }
                    parse_error
                };
                unsafe { self.as_mut().end_reset_model() };
                let count = self.params.len() as i32;
                self.as_mut().set_name(qs(&profile.name));
                self.as_mut().set_preset_path(preset);
                self.as_mut().set_parse_error(parse_error);
                self.as_mut().set_count(count);
            }
            Err(e) => self.as_mut().set_error(qs(format!("{}: {e}", path.display()))),
        }
        self.as_mut().set_open(true);
    }

    fn reparse(mut self: Pin<&mut Self>) {
        let preset = self.preset_path.to_string();
        let before = self.parsed_preset.clone();
        // Safety: paired with `end_reset_model` below.
        unsafe { self.as_mut().begin_reset_model() };
        let parse_error = self.as_mut().rust_mut().reparse(&preset);
        unsafe { self.as_mut().end_reset_model() };
        if before != self.parsed_preset {
            // A new preset means a new chain; drop the old one so the
            // next render reloads it.
            self.as_mut().rust_mut().preview = None;
        }
        let count = self.params.len() as i32;
        self.as_mut().set_parse_error(parse_error);
        self.as_mut().set_count(count);
    }

    fn set_override(mut self: Pin<&mut Self>, row: i32, enabled: bool) {
        let row = row as usize;
        // Safety: paired with `end_reset_model` below.
        unsafe { self.as_mut().begin_reset_model() };
        {
            let mut this = self.as_mut().rust_mut();
            if let (Some(meta), Some(over)) = (this.params.get(row), this.overrides.get(row)) {
                let new = if enabled { Some(meta.default) } else { None };
                if *over != new {
                    this.overrides[row] = new;
                }
            }
        }
        unsafe { self.as_mut().end_reset_model() };
    }

    fn set_value(mut self: Pin<&mut Self>, row: i32, value: f32) {
        let mut this = self.as_mut().rust_mut();
        let row = row as usize;
        // Only an *overridden* row can move: a disabled slider still
        // reports a value, and several presets have defaults off their
        // own step grid (crt-lottes: warpX 0.031, step 0.01), so without
        // this guard just opening such a preset would silently override
        // them with the snapped value.
        if this.overrides.get(row).map(Option::is_some).unwrap_or(false) {
            this.overrides[row] = Some(value);
        }
    }

    fn render(mut self: Pin<&mut Self>, area_w: i32, area_h: i32) {
        let preset = self.preset_path.to_string().trim().to_string();
        let image = self.preview_image.to_string().trim().to_string();
        if preset.is_empty() || image.is_empty() {
            return;
        }
        let params = self.rust().effective();
        let (source, w, h, err) = {
            let mut this = self.as_mut().rust_mut();
            let preview = this.preview.get_or_insert_with(preview::Preview::new);
            preview.update(
                Path::new(&preset),
                &params,
                Path::new(&image),
                area_w.max(1) as u32,
                area_h.max(1) as u32,
            );
            let (w, h) = preview.viewport();
            (qs(preview.source_url()), w as i32, h as i32, preview.error().map(qs).unwrap_or_default())
        };
        self.as_mut().set_preview_source(source);
        self.as_mut().set_preview_width(w);
        self.as_mut().set_preview_height(h);
        self.as_mut().set_error(err);
    }

    fn save(mut self: Pin<&mut Self>, profiles_dir: &QString) -> bool {
        let name = self.name.to_string();
        let preset = self.preset_path.to_string();
        if name.trim().is_empty() {
            self.as_mut().set_error(QString::from("a name is required"));
            return false;
        }
        if preset.trim().is_empty() {
            self.as_mut().set_error(QString::from("a preset is required"));
            return false;
        }
        let dir = PathBuf::from(profiles_dir.to_string());
        let profile = self.rust().build(name, PathBuf::from(preset));
        let result = match &self.path {
            Some(path) => profile.save(path),
            // `create` reserves the `<slug>.toml` and writes a bare
            // profile; the overrides the editor collected go into the
            // same file straight after.
            None => shader_library::create(&dir, profile.name.clone(), profile.preset.clone())
                .and_then(|path| profile.save(&path)),
        };
        match result {
            Ok(()) => {
                self.as_mut().set_error(QString::default());
                self.as_mut().set_open(false);
                true
            }
            Err(e) => {
                self.as_mut().set_error(qs(e));
                false
            }
        }
    }

    fn download_presets(mut self: Pin<&mut Self>) {
        let dest = shader_source::install_dir();
        self.as_mut().rust_mut().download = Some(Download::start(dest));
        self.as_mut().set_download_state(QString::from("running:0.0"));
    }

    fn poll_download(mut self: Pin<&mut Self>) {
        let status = self.rust().download.as_ref().map(Download::status);
        let Some(status) = status else { return };
        match status {
            Status::Running(bytes) => {
                let mb = bytes as f64 / 1_000_000.0;
                self.as_mut().set_download_state(qs(format!("running:{mb:.1}")));
            }
            Status::Done(dir) => {
                {
                    let mut this = self.as_mut().rust_mut();
                    this.download = None;
                    this.looked_for_presets = true;
                }
                let d = qs(dir.display());
                self.as_mut().set_presets_dir(d);
                self.as_mut().set_download_state(QString::default());
            }
            Status::Failed(e) => {
                self.as_mut().rust_mut().download = None;
                self.as_mut().set_download_state(qs(format!("failed:{e}")));
            }
        }
    }
}

impl ffi::ShaderEditor {
    /// Back to an empty editor. Every Q_PROPERTY goes through its own
    /// setter — clearing one by assigning the struct field would leave
    /// QML showing the last profile's name with no notify to correct it
    /// (see the header of `wizard.rs`).
    fn reset(mut self: Pin<&mut Self>) {
        // Safety: paired with `end_reset_model` below.
        unsafe { self.as_mut().begin_reset_model() };
        self.as_mut().rust_mut().clear();
        unsafe { self.as_mut().end_reset_model() };
        self.as_mut().set_count(0);
        self.as_mut().set_name(QString::default());
        self.as_mut().set_preset_path(QString::default());
        self.as_mut().set_preview_image(QString::default());
        self.as_mut().set_preview_source(QString::default());
        self.as_mut().set_preview_width(0);
        self.as_mut().set_preview_height(0);
        self.as_mut().set_parse_error(QString::default());
        self.as_mut().set_error(QString::default());
        self.as_mut().set_download_state(QString::default());
    }
}
