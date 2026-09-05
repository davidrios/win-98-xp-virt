//! The shader profile manager window: list existing profiles (New / Edit /
//! Delete), and an editor that picks a `.slangp` preset and exposes its
//! parameters (`shader_profile::parameter_meta`) as sliders — each one
//! optionally overridden, the rest left at the preset's own default.
//! Mirrors `wizard.rs`'s shape (an `open` flag, a `show` that returns
//! `Some` once something changed so the caller rescans).

use crate::filepicker;
use crate::shader_library;
use crate::shader_preview::Preview;
use crate::shader_profile::{self, ParamMeta, ShaderProfile};
use eframe::egui_wgpu::RenderState;
use std::path::{Path, PathBuf};

const PRESET_FILTER: filepicker::Filter = ("Shader presets", &["slangp"]);
const IMAGE_FILTER: filepicker::Filter = ("Images", &["png", "jpg", "jpeg", "bmp"]);

struct Editor {
    /// `None` for a new profile; `Some(path)` to save back in place.
    path: Option<PathBuf>,
    name: String,
    preset_path: String,
    /// The last preset path successfully parsed, so parameters are only
    /// re-read (and slider state re-derived) when it actually changes —
    /// not on every frame the field is drawn.
    parsed_preset: Option<PathBuf>,
    params: Vec<ParamMeta>,
    parse_error: Option<String>,
    /// One entry per `params`, in the same order: `Some(value)` when
    /// overridden. Indexed in lockstep with `params` rather than keyed by
    /// name so a slider drag doesn't need a map lookup per frame.
    overrides: Vec<Option<f32>>,
    /// A screenshot to preview the shader against; built lazily (needs a
    /// wgpu render backend, which isn't guaranteed to exist) on first use.
    preview_image_path: String,
    preview: Option<Preview>,
    /// Whether the editor window is currently blown up to (roughly) the
    /// full screen, for a big, "how it'll really look in the player"
    /// preview — see `ShaderManager::show`.
    fullscreen: bool,
    error: Option<String>,
}

impl Editor {
    fn fresh() -> Editor {
        Editor {
            path: None,
            name: String::new(),
            preset_path: String::new(),
            parsed_preset: None,
            params: Vec::new(),
            parse_error: None,
            overrides: Vec::new(),
            preview_image_path: String::new(),
            preview: None,
            fullscreen: false,
            error: None,
        }
    }

    fn from_profile(path: PathBuf, profile: &ShaderProfile) -> Editor {
        let mut e = Editor {
            path: Some(path),
            name: profile.name.clone(),
            preset_path: profile.preset.display().to_string(),
            ..Editor::fresh()
        };
        e.reparse();
        // Line up `overrides` with the freshly parsed `params`, seeded
        // from the saved profile — after `reparse` so a preset that
        // dropped a parameter since the profile was saved doesn't leave
        // a dangling override.
        if let Some(saved) = Some(&profile.params) {
            for (meta, over) in e.params.iter().zip(e.overrides.iter_mut()) {
                if let Some(&v) = saved.get(&meta.id) {
                    *over = Some(v);
                }
            }
        }
        e
    }

    fn reparse(&mut self) {
        let path = Path::new(self.preset_path.trim());
        if self.preset_path.trim().is_empty() {
            self.params.clear();
            self.overrides.clear();
            self.parsed_preset = None;
            self.parse_error = None;
            return;
        }
        if self.parsed_preset.as_deref() == Some(path) {
            return;
        }
        match shader_profile::parameter_meta(path) {
            Ok(params) => {
                self.overrides = vec![None; params.len()];
                self.params = params;
                self.parse_error = None;
            }
            Err(e) => {
                self.params.clear();
                self.overrides.clear();
                self.parse_error = Some(e);
            }
        }
        self.parsed_preset = Some(path.to_path_buf());
    }

    fn build(&self) -> ShaderProfile {
        let mut profile = ShaderProfile::new(self.name.clone(), self.preset_path.clone().into());
        for (meta, over) in self.params.iter().zip(self.overrides.iter()) {
            if let Some(v) = over {
                profile.params.insert(meta.id.clone(), *v);
            }
        }
        profile
    }
}

#[derive(Default)]
pub struct ShaderManager {
    pub open: bool,
    editor: Option<Editor>,
}

impl ShaderManager {
    pub fn open_list(&mut self) {
        self.open = true;
        self.editor = None;
    }

    /// Opens the editor directly, pre-filled with a preset and preview
    /// image (and optionally fullscreen) — a debug hook
    /// (`LAUNCHER_DEBUG_SHADER_PREVIEW=preset;image[;fullscreen]` in
    /// `main.rs`) to screenshot the *real* windowed editor without a GUI
    /// click, since this session has no click automation.
    pub fn debug_open_editor(&mut self, preset_path: String, preview_image_path: String, fullscreen: bool) {
        let mut editor = Editor::fresh();
        editor.preset_path = preset_path;
        editor.preview_image_path = preview_image_path;
        editor.fullscreen = fullscreen;
        editor.reparse();
        self.editor = Some(editor);
        self.open = true;
    }

    /// Renders the manager if open. `render_state` is eframe's wgpu
    /// context (`None` on a non-wgpu backend, e.g. web/glow — the editor
    /// then shows the sliders without a live preview rather than
    /// panicking). Returns `Some(())` once a profile was created, saved,
    /// or deleted, so the caller rescans the library.
    pub fn show(&mut self, ctx: &egui::Context, profiles_dir: &Path, render_state: Option<&RenderState>) -> Option<()> {
        if !self.open {
            return None;
        }
        let mut changed = None;
        let mut still_open = true;
        // "Fullscreen": blows the window up to the screen so the preview
        // pane (which fills whatever's left of the window after the
        // fixed-width controls column, see `editor_ui`) gets much bigger
        // — closer to actually seeing it "the way the player would show
        // it". `resizable` always on so a non-fullscreen user can still
        // grow it a bit by hand; `max_size` (rather than nothing) is what
        // makes toggling fullscreen back *off* actually shrink it again —
        // egui remembers a window's last rect across frames, and without
        // this it would just stay at the fullscreen size once it had been there.
        let fullscreen = self.editor.as_ref().is_some_and(|e| e.fullscreen);
        let mut window = egui::Window::new("Shader profiles").open(&mut still_open).collapsible(false).resizable(true);
        window = if fullscreen {
            window.fixed_rect(ctx.viewport_rect())
        } else {
            window.default_width(760.0).max_size(egui::vec2(900.0, 700.0))
        };
        window.show(ctx, |ui| {
                if let Some(editor) = &mut self.editor {
                    editor.reparse();
                    match editor_ui(ui, editor, profiles_dir, render_state) {
                        EditorAction::None => {}
                        EditorAction::Saved => {
                            changed = Some(());
                            self.editor = None;
                        }
                        EditorAction::Cancelled => self.editor = None,
                    }
                    return;
                }
                let entries = shader_library::scan(profiles_dir);
                if entries.is_empty() {
                    ui.label("No shader profiles yet.");
                } else {
                    egui::Grid::new("shader-profiles").striped(true).show(ui, |ui| {
                        ui.strong("Name");
                        ui.strong("Preset");
                        ui.strong("");
                        ui.end_row();
                        for entry in &entries {
                            ui.label(&entry.profile.name);
                            ui.label(entry.profile.preset.display().to_string());
                            ui.horizontal(|ui| {
                                if ui.button("Edit…").clicked() {
                                    self.editor = Some(Editor::from_profile(entry.path.clone(), &entry.profile));
                                }
                                if ui.button("Delete").clicked() {
                                    if let Err(e) = shader_library::delete(&entry.path) {
                                        eprintln!("[shader-manager] deleting {}: {e}", entry.path.display());
                                    }
                                    changed = Some(());
                                }
                            });
                            ui.end_row();
                        }
                    });
                }
                ui.add_space(8.0);
                if ui.button("New profile…").clicked() {
                    self.editor = Some(Editor::fresh());
                }
            });
        if changed.is_some() {
            return changed;
        }
        self.open = still_open;
        None
    }
}

enum EditorAction {
    None,
    Saved,
    Cancelled,
}

/// The profile editor form. `Saved` once "Save" wrote the profile,
/// `Cancelled` once "Cancel" was clicked — either closes the editor, but
/// only `Saved` should make the caller rescan the library.
fn editor_ui(ui: &mut egui::Ui, editor: &mut Editor, profiles_dir: &Path, render_state: Option<&RenderState>) -> EditorAction {
    let mut action = EditorAction::None;
    ui.horizontal(|ui| {
        ui.label("Name");
        ui.text_edit_singleline(&mut editor.name);
    });
    filepicker::path_field(ui, "Preset (.slangp)", &mut editor.preset_path, Some(PRESET_FILTER));
    ui.separator();
    // A fixed-width controls column, the rest of the window (however
    // big — see the "Fullscreen" toggle in `ShaderManager::show`) for
    // the preview: growing the window grows the preview, not the
    // sliders, since a bigger *preview* is the whole point of going
    // fullscreen (the user's own ask: "use the remaining horizontal
    // space for the shader controls" once the image no longer needs it).
    const CONTROLS_WIDTH: f32 = 300.0;
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.set_width(CONTROLS_WIDTH);
            if let Some(err) = &editor.parse_error {
                ui.colored_label(egui::Color32::RED, format!("Couldn't read this preset's parameters: {err}"));
            } else if editor.params.is_empty() {
                ui.label("Pick a preset to see its parameters.");
            } else {
                egui::ScrollArea::vertical().id_salt("params").show(ui, |ui| {
                    for (meta, over) in editor.params.iter().zip(editor.overrides.iter_mut()) {
                        ui.horizontal(|ui| {
                            let mut enabled = over.is_some();
                            if ui.checkbox(&mut enabled, "").changed() {
                                *over = if enabled { Some(meta.default) } else { None };
                            }
                            ui.add_enabled_ui(enabled, |ui| {
                                let mut value = over.unwrap_or(meta.default);
                                let resp = ui.add(
                                    egui::Slider::new(&mut value, meta.minimum..=meta.maximum)
                                        .step_by(if meta.step > 0.0 { meta.step as f64 } else { 0.0 })
                                        .text(&meta.id),
                                );
                                if resp.changed() {
                                    *over = Some(value);
                                }
                            });
                        });
                        if !meta.description.is_empty() && meta.description != meta.id {
                            ui.label(egui::RichText::new(&meta.description).weak().small());
                        }
                    }
                });
            }
        });
        ui.separator();
        ui.vertical(|ui| preview_ui(ui, editor, render_state));
    });
    ui.separator();
    if let Some(err) = &editor.error {
        ui.colored_label(egui::Color32::RED, err);
    }
    ui.horizontal(|ui| {
        if ui.button("Save").clicked() {
            if editor.name.trim().is_empty() {
                editor.error = Some("a name is required".into());
            } else if editor.preset_path.trim().is_empty() {
                editor.error = Some("a preset is required".into());
            } else {
                let profile = editor.build();
                let result = match &editor.path {
                    Some(path) => profile.save(path),
                    None => shader_library::create(profiles_dir, profile.name.clone(), profile.preset.clone()).map(|_| ()),
                };
                match result {
                    Ok(()) => action = EditorAction::Saved,
                    Err(e) => editor.error = Some(e.to_string()),
                }
            }
        }
        if ui.button("Cancel").clicked() {
            action = EditorAction::Cancelled;
        }
    });
    action
}

/// The live preview column: an image picker plus, once both a valid
/// preset and an image are chosen, the shader's effect on it — re-run
/// every time this is called with the sliders' current values, so
/// dragging one updates the picture live. Rendered integer-scaled and
/// letterboxed exactly like `player::Gpu::viewport` (`shader_preview.rs`
/// does the actual scale/render math); this just reserves the area and
/// paints the result centered in it, black behind — "how it'll really
/// look in the player", not an image widget stretched to fit.
fn preview_ui(ui: &mut egui::Ui, editor: &mut Editor, render_state: Option<&RenderState>) {
    ui.horizontal(|ui| {
        ui.label("Preview");
        ui.checkbox(&mut editor.fullscreen, "Fullscreen");
    });
    filepicker::path_field(ui, "Image", &mut editor.preview_image_path, Some(IMAGE_FILTER));
    let Some(render_state) = render_state else {
        ui.label("(no wgpu render backend — live preview unavailable)");
        return;
    };
    if editor.preview_image_path.trim().is_empty() {
        ui.label("Pick a screenshot to preview the shader on it.");
        return;
    }
    if editor.parse_error.is_some() || editor.params.is_empty() {
        return; // nothing valid to render yet; the error already shows on the left
    }

    // Everything left of the window/screen (however big) after the
    // fields above and the controls column beside us — floored to a
    // reasonable minimum so a compact (non-fullscreen) window doesn't
    // just shrink the preview down to whatever little space is left
    // over (egui auto-sizes an unconstrained window to its content,
    // not the other way around: asking for at least this much here is
    // what keeps the window itself from collapsing too small to show
    // the image un-cropped).
    let avail = ui.available_size();
    let area = egui::vec2(avail.x.max(480.0), avail.y.max(360.0));

    let preview = editor.preview.get_or_insert_with(|| Preview::new(render_state.clone()));
    let effective: Vec<(String, f32)> =
        editor.params.iter().zip(&editor.overrides).map(|(meta, over)| (meta.id.clone(), over.unwrap_or(meta.default))).collect();
    preview.update(Path::new(editor.preset_path.trim()), &effective, Path::new(editor.preview_image_path.trim()), area);
    if let Some(err) = preview.error() {
        ui.colored_label(egui::Color32::RED, err);
    }

    let (rect, _response) = ui.allocate_exact_size(area, egui::Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 0.0, egui::Color32::BLACK);
    if let Some(id) = preview.texture_id() {
        let image_rect = egui::Rect::from_center_size(rect.center(), preview.viewport_size());
        painter.image(id, image_rect, egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)), egui::Color32::WHITE);
    }
}
