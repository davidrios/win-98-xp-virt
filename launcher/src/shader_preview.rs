//! Live preview for the shader profile editor: a still image (a game
//! screenshot, typically) run through the same `shader-chain` crate the
//! player uses, displayed as an egui texture and re-rendered whenever
//! the preset, its parameter values, or the preview image change.
//!
//! Runs on the `wgpu::Device`/`Queue` eframe itself already opened to
//! draw egui (`egui_wgpu::RenderState`, via `eframe::wgpu`/
//! `eframe::egui_wgpu`) — no second GPU context, unlike the player
//! (which is a whole separate process with its own window).

use eframe::egui_wgpu::RenderState;
use eframe::{egui, wgpu};
use std::path::{Path, PathBuf};

/// A source bigger than this is downsized on the CPU before the shader
/// ever sees it — generous enough to pass any real game resolution
/// through untouched (this is meant to *look like the player*: a native
/// game resolution treated as-is, then integer-scaled up exactly the way
/// `player::Gpu::viewport` does), it's only a sanity cap against
/// rendering, say, a 12-megapixel phone photo at full size every frame.
const MAX_SOURCE_W: f32 = 1600.0;
const MAX_SOURCE_H: f32 = 1200.0;

pub struct Preview {
    render_state: RenderState,
    image_path: Option<PathBuf>,
    input: Option<(wgpu::Texture, u32, u32)>,
    preset_path: Option<PathBuf>,
    chain: Option<shader_chain::Chain>,
    texture_id: Option<egui::TextureId>,
    viewport_size: egui::Vec2,
    error: Option<String>,
}

impl Preview {
    pub fn new(render_state: RenderState) -> Preview {
        Preview {
            render_state,
            image_path: None,
            input: None,
            preset_path: None,
            chain: None,
            texture_id: None,
            viewport_size: egui::Vec2::ZERO,
            error: None,
        }
    }

    /// The egui texture to display, once at least one frame has rendered
    /// successfully.
    pub fn texture_id(&self) -> Option<egui::TextureId> {
        self.texture_id
    }

    /// The exact size (in the same units as `update`'s `area`) the last
    /// rendered frame came out at: the input image's own size times the
    /// largest *integer* factor that fits `area` — never a fraction, so
    /// the shader is never blurred by a second, non-integer resample the
    /// way stretching it to fill `area` exactly would. Smaller than
    /// `area` on one axis (letterboxed) unless the aspect ratios happen
    /// to match exactly; the caller centers it and fills the rest.
    pub fn viewport_size(&self) -> egui::Vec2 {
        self.viewport_size
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// The last rendered frame, for a caller that wants to read it back
    /// itself (`main.rs`'s `--preview-shader` debug verb, to prove this
    /// module's image-decode-and-render path headlessly without a GUI
    /// click, dumping it the same way the player's `PLAYER_DUMP_OUT` does).
    pub fn output_texture(&self) -> Option<&wgpu::Texture> {
        self.chain.as_ref().and_then(shader_chain::Chain::output_texture)
    }

    /// Reflect the editor's current preset path, effective parameter
    /// values (defaults already merged with overrides — this module
    /// doesn't need to know which is which), preview image path, and the
    /// area available to render into (the caller's layout, so this can
    /// grow with the editor window — e.g. a "fullscreen preview" toggle)
    /// — reloading only what actually changed and re-rendering a frame.
    pub fn update(&mut self, preset: &Path, params: &[(String, f32)], image: &Path, area: egui::Vec2) {
        if self.image_path.as_deref() != Some(image) {
            self.load_image(image);
        }
        if self.preset_path.as_deref() != Some(preset) {
            self.load_preset(preset);
        }
        if self.input.is_none() || self.chain.is_none() {
            return;
        }
        self.error = None;
        if let Some(chain) = &self.chain {
            chain.set_parameters(params);
        }
        self.render(area);
    }

    fn load_image(&mut self, path: &Path) {
        self.image_path = Some(path.to_path_buf());
        self.input = None;
        let img = match image::open(path) {
            Ok(img) => img,
            Err(e) => {
                self.error = Some(format!("loading {}: {e}", path.display()));
                return;
            }
        };
        // A CRT preset is written to *upscale* a small native-resolution
        // source, never to shrink one: several (crt-aperture.slang, e.g.)
        // compute `scale = floor(OutputSize / SourceSize)` and then
        // divide by it — 0 when the source is bigger than the render
        // target, i.e. NaN, i.e. a solid black frame (found from a real
        // report: a 1025x791 photo through crt-aperture rendered black).
        // `render`'s own `.max(1.0)` on the scale now guarantees that
        // can't happen regardless of this cap (see there) — this is just
        // a sanity limit against treating an arbitrarily huge photo as
        // "native resolution" and rendering it at full size every frame.
        let img = if img.width() > MAX_SOURCE_W as u32 || img.height() > MAX_SOURCE_H as u32 {
            img.resize(MAX_SOURCE_W as u32, MAX_SOURCE_H as u32, image::imageops::FilterType::Triangle)
        } else {
            img
        };
        let img = img.to_rgba8();
        let (w, h) = img.dimensions();
        let device = &self.render_state.device;
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("shader preview input"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            // Not sRGB: the shader's own GAMMA_INPUT parameter (present
            // on most CRT presets) does that math itself, same as the
            // player feeding it raw (non-sRGB-tagged) guest pixels.
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        self.render_state.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            img.as_raw(),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(4 * w), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.input = Some((tex, w, h));
    }

    fn load_preset(&mut self, path: &Path) {
        self.preset_path = Some(path.to_path_buf());
        self.chain = None;
        // egui_wgpu::Renderer::register_native_texture requires exactly
        // this format for a texture it's handed.
        match shader_chain::Chain::load(
            path,
            &self.render_state.device,
            &self.render_state.queue,
            self.render_state.adapter.get_info(),
            wgpu::TextureFormat::Rgba8Unorm,
        ) {
            Ok(c) => self.chain = Some(c),
            Err(e) => self.error = Some(format!("loading shader: {e}")),
        }
    }

    fn render(&mut self, area: egui::Vec2) {
        let Some((_, iw, ih)) = &self.input else { return };
        let (iw, ih) = (*iw, *ih);
        if self.chain.is_none() {
            return;
        }
        // Exactly `player::Gpu::viewport`'s own math: the largest
        // *integer* scale that fits `area`, floored (never a fraction —
        // a non-integer scale would blur the very thing being
        // previewed) and never below 1 (never shrunk — the source is
        // already capped to `MAX_SOURCE_W/H` in `load_image`, but this
        // is also what keeps some presets' own internal
        // `1.0 / floor(OutputSize/SourceSize)` away from a division by
        // zero regardless of that cap). Below 1 in `area` itself, the
        // rendered frame is bigger than `area` and the caller crops it
        // around the center — same as making the real player's window
        // smaller than the guest's native resolution.
        let (aw, ah) = (area.x.max(1.0), area.y.max(1.0));
        let scale = (aw / iw as f32).min(ah / ih as f32).floor().max(1.0);
        let (rw, rh) = ((iw as f32 * scale) as u32, (ih as f32 * scale) as u32);
        self.viewport_size = egui::Vec2::new(rw as f32, rh as f32);

        let device = self.render_state.device.clone();
        let queue = self.render_state.queue.clone();
        let mut encoder =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shader preview") });
        let result = {
            let input_tex = &self.input.as_ref().unwrap().0;
            let chain = self.chain.as_mut().unwrap();
            chain.run(&device, &mut encoder, input_tex, rw, rh)
        };
        match result {
            Ok((_, view)) => {
                queue.submit(Some(encoder.finish()));
                let mut renderer = self.render_state.renderer.write();
                match self.texture_id {
                    Some(id) => {
                        renderer.update_egui_texture_from_wgpu_texture(&device, view, wgpu::FilterMode::Linear, id)
                    }
                    None => {
                        self.texture_id =
                            Some(renderer.register_native_texture(&device, view, wgpu::FilterMode::Linear))
                    }
                }
            }
            Err(e) => self.error = Some(e),
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
