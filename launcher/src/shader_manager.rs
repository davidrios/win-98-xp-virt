//! The shader profile manager window, in egui: the profile list (New /
//! Edit / Delete) and the editor that picks a `.slangp` and exposes its
//! parameters as sliders.
//!
//! The editor's behaviour — reading a preset, which parameters are
//! overridden, the "only a drag counts" guard, what `save` writes — is
//! `launcher_core::editor::Editor`, and the preset collection with its
//! download is `launcher_core::editor::Presets`. What is here is the
//! window: its two screens, the "Fullscreen" toggle, and the panel
//! layout that makes it resizable at all.

use crate::filepicker;
use crate::shader_preview::Preview;
use launcher_core::editor::{Editor, PresetState, Presets, IMAGE_FILTER, PRESET_FILTER};
use launcher_core::shader_library;
use eframe::egui_wgpu::RenderState;
use std::path::Path;

#[derive(Default)]
pub struct ShaderManager {
    pub open: bool,
    /// `Some` while the editor screen is up; `None` on the profile list.
    editor: Option<EditorView>,
    presets: Presets,
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

/// The editor model plus the two things only this toolkit needs: a live
/// preview built on eframe's own GPU context, and the "Fullscreen"
/// toggle. (The Qt build needs neither — its editor is a real top-level
/// window the user drags as wide as they like.)
#[derive(Default)]
struct EditorView {
    model: Editor,
    /// Built lazily: it needs a wgpu render backend, which isn't
    /// guaranteed to exist.
    preview: Option<Preview>,
    fullscreen: bool,
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

    /// Open the editor directly, pre-filled with a preset and preview
    /// image (and optionally fullscreen) — a debug hook
    /// (`LAUNCHER_DEBUG_SHADER_PREVIEW=preset;image[;fullscreen]` in
    /// `main.rs`) to screenshot the *real* windowed editor without a GUI
    /// click, since this session has no click automation.
    pub fn debug_open_editor(&mut self, preset_path: String, preview_image_path: String, fullscreen: bool) {
        let mut view = EditorView { fullscreen, ..Default::default() };
        view.model.open_with(preset_path, preview_image_path);
        self.editor = Some(view);
        self.open = true;
    }

    /// Renders the manager if open. `render_state` is eframe's wgpu
    /// context (`None` on a non-wgpu backend, e.g. web/glow — the editor
    /// then shows the sliders without a live preview rather than
    /// panicking). Returns `Some(())` once a profile was created, saved
    /// or deleted, so the caller rescans the library.
    pub fn show(&mut self, ctx: &egui::Context, profiles_dir: &Path, render_state: Option<&RenderState>) -> Option<()> {
        if !self.open {
            return None;
        }
        let mut changed = None;
        let mut still_open = true;
        // "Fullscreen" blows the window up to the screen so the preview
        // pane (which fills whatever's left of the window after the
        // fixed-width controls column) gets much bigger — closer to
        // actually seeing it "the way the player would show it".
        // Otherwise the window is freely resizable in *both* directions:
        // `default_size` (not just `default_width`) so it opens tall
        // enough for the preview and the sliders, and `editor_ui` lays
        // its body out with panels that fill whatever height the window
        // has — without that, egui sizes the window to its content's own
        // height and dragging the bottom edge does nothing.
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
            if let Some(view) = &mut self.editor {
                view.model.reparse();
                match editor_ui(ui, view, profiles_dir, render_state, &mut self.presets) {
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
                                let mut view = EditorView::default();
                                view.model.edit(entry.path.clone(), &entry.profile);
                                self.editor = Some(view);
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
                let mut view = EditorView::default();
                view.model.new_profile();
                self.editor = Some(view);
            }
            presets_ui(ui, &mut self.presets);
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

/// The preset-collection row, on both screens: nothing at all while a
/// collection is on disk (the preset picker just works, and there is
/// nothing to decide), a "Download presets" button when there is none,
/// and the download's own progress while it runs.
///
/// Shown on both the profile list and inside the editor because that is
/// where each question is asked — the list is where someone discovers
/// they have no shaders at all, the editor is where an empty preset
/// field stops them mid-profile.
fn presets_ui(ui: &mut egui::Ui, presets: &mut Presets) {
    match presets.state() {
        PresetState::Ready(_) => {}
        PresetState::Downloading(mb) => {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(format!("Downloading shader presets… {mb:.1} MB"));
            });
            // The download runs on its own thread and nothing else would
            // wake the UI to show it moving.
            ui.ctx().request_repaint();
        }
        PresetState::Failed(err) => {
            ui.colored_label(egui::Color32::RED, format!("Couldn't download the shader presets: {err}"));
            if ui.button("Try again").clicked() {
                presets.start_download();
            }
        }
        PresetState::Missing { install_dir, size } => {
            ui.label("No shader presets on this machine — a profile needs a .slangp to build on.");
            if ui.button(format!("Download presets ({size})")).clicked() {
                presets.start_download();
            }
            ui.label(
                egui::RichText::new(format!("libretro's slang-shaders, into {}", install_dir.display()))
                    .weak()
                    .small(),
            );
        }
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
fn editor_ui(
    ui: &mut egui::Ui,
    view: &mut EditorView,
    profiles_dir: &Path,
    render_state: Option<&RenderState>,
    presets: &mut Presets,
) -> EditorAction {
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
            ui.text_edit_singleline(&mut view.model.name);
        });
        // "Browse…" on an empty field opens in the preset collection
        // rather than the OS default: a `.slangp` lives in a checkout's
        // `third_party/` or a downloaded copy in a data directory, and
        // neither is anywhere a person would navigate to by hand.
        filepicker::path_field_in(
            ui,
            "Preset (.slangp)",
            &mut view.model.preset_path,
            Some(PRESET_FILTER),
            presets.dir().as_deref(),
        );
        presets_ui(ui, presets);
        ui.separator();
    });
    egui::Panel::bottom("shader-editor-foot").frame(egui::Frame::NONE).show_separator_line(false).show(ui, |ui| {
        ui.separator();
        if let Some(err) = &view.model.error {
            ui.colored_label(egui::Color32::RED, err);
        }
        ui.horizontal(|ui| {
            if ui.button("Save").clicked() && view.model.save(profiles_dir) {
                action = EditorAction::Saved;
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
    // fullscreen. Growing it *taller* grows both: the sliders get more
    // of their list visible at once, the preview a taller area to be
    // scaled into.
    const CONTROLS_WIDTH: f32 = 300.0;
    egui::CentralPanel::default().frame(egui::Frame::NONE).show(ui, |ui| {
        ui.horizontal_top(|ui| {
            let body_height = ui.available_height();
            let layout = egui::Layout::top_down(egui::Align::Min);
            ui.allocate_ui_with_layout(egui::vec2(CONTROLS_WIDTH, body_height), layout, |ui| {
                params_ui(ui, &mut view.model);
            });
            ui.separator();
            ui.vertical(|ui| preview_ui(ui, view, render_state));
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
    if let Some(err) = editor.parse_error() {
        ui.colored_label(egui::Color32::RED, format!("Couldn't read this preset's parameters: {err}"));
        return;
    }
    if editor.params().is_empty() {
        ui.label("Pick a preset to see its parameters.");
        return;
    }
    egui::ScrollArea::vertical().id_salt("params").auto_shrink([false, false]).show(ui, |ui| {
        for row in 0..editor.params().len() {
            let meta = &editor.params()[row];
            let (id, description) = (meta.id.clone(), meta.description.clone());
            let (minimum, maximum, step, default) = (meta.minimum, meta.maximum, meta.step, meta.default);
            let mut enabled = editor.is_overridden(row);
            let mut value = editor.value(row).unwrap_or(default);
            ui.horizontal(|ui| {
                if ui.checkbox(&mut enabled, "").changed() {
                    editor.set_override(row, enabled);
                }
                ui.add_enabled_ui(enabled, |ui| {
                    // Step only where the user is actually editing, so an
                    // un-overridden parameter keeps showing the preset's
                    // own default even when that sits off the step grid.
                    let step = if enabled && step > 0.0 { step as f64 } else { 0.0 };
                    let resp = ui.add(egui::Slider::new(&mut value, minimum..=maximum).step_by(step).text(&id));
                    if resp.changed() {
                        // `set_value` ignores a row that isn't
                        // overridden, which is what stops a greyed-out
                        // slider's step-snapped value from becoming one.
                        editor.set_value(row, value);
                    }
                });
            });
            if !description.is_empty() && description != id {
                ui.label(egui::RichText::new(&description).weak().small());
            }
        }
    });
}

/// The live preview column: an image picker plus, once both a valid
/// preset and an image are chosen, the shader's effect on it — re-run
/// every time this is called with the sliders' current values, so
/// dragging one updates the picture live. Rendered integer-scaled and
/// letterboxed exactly like `player::Gpu::viewport`; this reserves the
/// area and paints the result centred in it, black behind — "how it'll
/// really look in the player", not an image widget stretched to fit.
fn preview_ui(ui: &mut egui::Ui, view: &mut EditorView, render_state: Option<&RenderState>) {
    ui.horizontal(|ui| {
        ui.label("Preview");
        ui.checkbox(&mut view.fullscreen, "Fullscreen");
    });
    filepicker::path_field(ui, "Image", &mut view.model.preview_image_path, Some(IMAGE_FILTER));
    let Some(render_state) = render_state else {
        ui.label("(no wgpu render backend — live preview unavailable)");
        return;
    };
    if view.model.preview_image_path.trim().is_empty() {
        ui.label("Pick a screenshot to preview the shader on it.");
        return;
    }
    if !view.model.renderable() {
        return; // nothing valid to render yet; the error already shows on the left
    }

    // Everything left of the window/screen (however big) after the
    // fields above and the controls column beside us. No floor: the
    // window's own `min_size` is what keeps this area usable, and asking
    // for more than is actually there would push the window wider/taller
    // every frame instead.
    let area = ui.available_size().max(egui::vec2(64.0, 64.0));

    let preview = view.preview.get_or_insert_with(|| Preview::new(render_state.clone()));
    preview.update(
        Path::new(view.model.preset_path.trim()),
        &view.model.effective(),
        Path::new(view.model.preview_image_path.trim()),
        area,
    );
    if let Some(err) = preview.error() {
        ui.colored_label(egui::Color32::RED, err.to_string());
    }

    let (rect, _response) = ui.allocate_exact_size(area, egui::Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 0.0, egui::Color32::BLACK);
    if let Some(id) = preview.texture_id() {
        let image_rect = egui::Rect::from_center_size(rect.center(), preview.viewport_size());
        painter.image(
            id,
            image_rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            egui::Color32::WHITE,
        );
    }
}
