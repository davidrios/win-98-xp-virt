//! The shader preview as egui sees it: `launcher_core::preview::Preview`
//! does the work — decode, chain, the integer-scale viewport math — and
//! this registers its output with `egui_wgpu` so the editor can paint it
//! by texture id.
//!
//! It runs on the `wgpu::Device`/`Queue` eframe itself already opened to
//! draw egui with (`egui_wgpu::RenderState`), so the rendered texture
//! reaches the widget with no copy at all. That is the one place this
//! front end is meaningfully better than the Qt one, which has to open a
//! second device and read the frame back through a file (doc 07).

use eframe::egui_wgpu::RenderState;
use eframe::{egui, wgpu};
use std::path::Path;

pub struct Preview {
    core: launcher_core::preview::Preview,
    render_state: RenderState,
    texture_id: Option<egui::TextureId>,
}

impl Preview {
    pub fn new(render_state: RenderState) -> Preview {
        let core = launcher_core::preview::Preview::new(
            render_state.device.clone(),
            render_state.queue.clone(),
            render_state.adapter.get_info(),
        );
        Preview { core, render_state, texture_id: None }
    }

    /// The egui texture to display, once at least one frame has rendered
    /// successfully.
    pub fn texture_id(&self) -> Option<egui::TextureId> {
        self.texture_id
    }

    /// The size the last rendered frame came out at, in points: the
    /// input image's own size times the largest *integer* factor that
    /// fits the area. The caller centres it and fills the rest.
    pub fn viewport_size(&self) -> egui::Vec2 {
        let (w, h) = self.core.viewport();
        egui::Vec2::new(w as f32, h as f32)
    }

    pub fn error(&self) -> Option<&str> {
        self.core.error()
    }

    /// How soon egui must repaint to keep an animated preset moving, or
    /// `None` when the picture stands still and the window can go back
    /// to sleep. The decision is the core's (`Preview::frame_interval`);
    /// this only translates it into `request_repaint_after`.
    pub fn frame_interval(&self) -> Option<std::time::Duration> {
        self.core.frame_interval()
    }

    /// Render one named frame from now on rather than following the
    /// clock — the headless dump, so that two runs of it are two
    /// identical PNGs even for a preset that animates.
    pub fn pin_frame(&mut self, frame: usize) {
        self.core.pin_frame(frame);
    }

    /// Reflect the editor's current preset, effective parameter values,
    /// image and available area, then hand the frame to egui.
    pub fn update(&mut self, preset: &Path, params: &[(String, f32)], image: &Path, area: egui::Vec2) {
        self.core.update(preset, params, image, area.x.max(1.0) as u32, area.y.max(1.0) as u32);
        let Some(view) = self.core.output_view() else { return };
        let device = &self.render_state.device;
        let mut renderer = self.render_state.renderer.write();
        match self.texture_id {
            Some(id) => renderer.update_egui_texture_from_wgpu_texture(device, view, wgpu::FilterMode::Linear, id),
            None => {
                self.texture_id = Some(renderer.register_native_texture(device, view, wgpu::FilterMode::Linear))
            }
        }
    }
}

impl Drop for Preview {
    fn drop(&mut self) {
        if let Some(id) = self.texture_id.take() {
            self.render_state.renderer.write().free_texture(&id);
        }
    }
}
