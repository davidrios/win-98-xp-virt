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
    /// The outer rect the window last had while *not* fullscreen, and —
    /// for one frame after the "Fullscreen" toggle goes back off — the
    /// rect to pin it back to. egui remembers a window's size across
    /// frames, and the fullscreen frames overwrite that memory with the
    /// whole screen; without this, un-fullscreening would leave the
    /// window screen-sized forever. (The size comes back; the window
    /// stays in the corner fullscreen moved it to — egui re-snaps the
    /// position from its own stored area state the frame after.)
    windowed_rect: Option<egui::Rect>,
    restore_rect: Option<egui::Rect>,
}

impl ShaderManager {
    /// Where the window ended up last frame while not fullscreen — for
    /// `main.rs`'s `--diag-editor-frame`, which has to aim a synthetic
    /// drag at its bottom edge.
    pub fn windowed_rect(&self) -> Option<egui::Rect> {
        self.windowed_rect
    }

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
        // it". Otherwise the window is freely resizable in *both*
        // directions: `default_size` (not just `default_width`) so it
        // opens tall enough for the preview and the sliders, and
        // `editor_ui` lays its body out with panels that fill whatever
        // height the window has — without that, egui sizes the window to
        // its content's own height and dragging the bottom edge does
        // nothing.
        let fullscreen = self.editor.as_ref().is_some_and(|e| e.fullscreen);
        // Separate ids for the two screens (egui keys a window's
        // remembered position and size by id): the editor wants to be
        // big and to stay wherever the user dragged it, the profile list
        // is a handful of rows and should stay small — sharing one id
        // would drag the editor's size onto the list.
        let editing = self.editor.is_some();
        let mut window = egui::Window::new("Shader profiles")
            .id(egui::Id::new(if editing { "shader-editor" } else { "shader-list" }))
            .open(&mut still_open)
            .collapsible(false)
            .resizable(true);
        window = if fullscreen {
            window.fixed_rect(ctx.viewport_rect())
        } else if let Some(rect) = self.restore_rect.take() {
            window.fixed_rect(rect)
        } else if editing {
            window
                .default_size(egui::vec2(980.0, 700.0))
                .min_size(egui::vec2(560.0, 360.0))
                .max_size(ctx.viewport_rect().size())
        } else {
            window.default_size(egui::vec2(560.0, 240.0)).max_size(ctx.viewport_rect().size())
        };
        let response = window.show(ctx, |ui| {
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
        if !fullscreen {
            self.windowed_rect = response.map(|r| r.response.rect);
        } else if !self.editor.as_ref().is_some_and(|e| e.fullscreen) {
            // The checkbox (or Save / Cancel) just turned fullscreen off:
            // put the window back where it was next frame.
            self.restore_rect = self.windowed_rect;
        }
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
    // The form is laid out as panels inside the window rather than as a
    // plain top-to-bottom stack: header and footer take their own
    // height, the body gets *everything* that's left. That's what makes
    // the window resizable vertically at all — an egui window is only as
    // tall as its content, so a stack of auto-sized widgets snaps back
    // the moment you let go of the bottom edge — and it's what keeps the
    // two body columns the same height however big the window is.
    egui::Panel::top("shader-editor-head").frame(egui::Frame::NONE).show_separator_line(false).show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label("Name");
            ui.text_edit_singleline(&mut editor.name);
        });
        filepicker::path_field(ui, "Preset (.slangp)", &mut editor.preset_path, Some(PRESET_FILTER));
        ui.separator();
    });
    egui::Panel::bottom("shader-editor-foot").frame(egui::Frame::NONE).show_separator_line(false).show(ui, |ui| {
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
    });
    // A fixed-width controls column, the rest of the window (however
    // big — see the "Fullscreen" toggle in `ShaderManager::show`) for
    // the preview: growing the window *wider* grows the preview, not the
    // sliders, since a bigger preview is the whole point of going
    // fullscreen (the user's own ask: "use the remaining horizontal
    // space for the shader controls" once the image no longer needs it).
    // Growing it *taller* grows both: the sliders get more of their list
    // visible at once, the preview a taller area to be scaled into.
    const CONTROLS_WIDTH: f32 = 300.0;
    egui::CentralPanel::default().frame(egui::Frame::NONE).show(ui, |ui| {
        ui.horizontal_top(|ui| {
            let body_height = ui.available_height();
            let layout = egui::Layout::top_down(egui::Align::Min);
            ui.allocate_ui_with_layout(egui::vec2(CONTROLS_WIDTH, body_height), layout, |ui| {
                params_ui(ui, editor);
            });
            ui.separator();
            ui.vertical(|ui| preview_ui(ui, editor, render_state));
        });
    });
    action
}

/// The parameter column: one checkbox ("override this one") plus a
/// slider per preset parameter, scrolling inside whatever height the
/// window has. `auto_shrink(false)` on both axes so the scroll area
/// really does take the full column — otherwise it sizes itself to its
/// content and a taller window leaves the sliders at their old height
/// with the scroll bar still there.
fn params_ui(ui: &mut egui::Ui, editor: &mut Editor) {
    if let Some(err) = &editor.parse_error {
        ui.colored_label(egui::Color32::RED, format!("Couldn't read this preset's parameters: {err}"));
        return;
    }
    if editor.params.is_empty() {
        ui.label("Pick a preset to see its parameters.");
        return;
    }
    egui::ScrollArea::vertical().id_salt("params").auto_shrink([false, false]).show(ui, |ui| {
        for (meta, over) in editor.params.iter().zip(editor.overrides.iter_mut()) {
            ui.horizontal(|ui| {
                let mut enabled = over.is_some();
                if ui.checkbox(&mut enabled, "").changed() {
                    *over = if enabled { Some(meta.default) } else { None };
                }
                ui.add_enabled_ui(enabled, |ui| {
                    let mut value = over.unwrap_or(meta.default);
                    // Step only where the user is actually editing, so an
                    // un-overridden parameter keeps showing the preset's
                    // own default even when that sits off the step grid.
                    let step = if enabled && meta.step > 0.0 { meta.step as f64 } else { 0.0 };
                    let resp =
                        ui.add(egui::Slider::new(&mut value, meta.minimum..=meta.maximum).step_by(step).text(&meta.id));
                    // Only a *drag* counts, hence the `enabled` guard: a
                    // greyed-out slider still snaps its value to the
                    // step and reports `changed()` for it, and several
                    // presets have defaults off the step grid
                    // (crt-lottes: warpX 0.031, step 0.01) — without
                    // this, just opening such a preset silently
                    // overrode those parameters with the snapped value.
                    if enabled && resp.changed() {
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
    // fields above and the controls column beside us. No floor: the
    // window's own `min_size` (`ShaderManager::show`) is what keeps this
    // area usable, and asking for more than is actually there would
    // push the window wider/taller every frame instead.
    let area = ui.available_size().max(egui::vec2(64.0, 64.0));

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
