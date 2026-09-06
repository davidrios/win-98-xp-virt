//! The shader profile editor's live preview, on Qt.
//!
//! The render half is identical to the egui build's `shader_preview.rs`
//! — same `shader-chain` crate, same decode, same "integer scale, then
//! letterbox" viewport math as `player::Gpu::viewport` — and it is
//! deliberately kept line-for-line comparable so a difference in the
//! output is a difference in this port, not in the shader.
//!
//! Two things had to change, and they are the interesting part of the
//! whole port:
//!
//! 1. **Whose GPU.** eframe hands egui a live `wgpu::Device`/`Queue`
//!    (`egui_wgpu::RenderState`) and the preview borrows it. Qt Quick
//!    renders through its own abstraction (QRhi) on Vulkan, and cxx-qt
//!    exposes no handle to it, so this opens a *second*, windowless wgpu
//!    device of its own. On this box that is a second Vulkan logical
//!    device on the same physical GPU: about 40 MB of extra VRAM and one
//!    more driver context, invisible in use, but it is a real cost that
//!    the egui build does not pay.
//!
//! 2. **How the frame reaches the widget.** egui takes the rendered
//!    texture by id — zero copy, it is already on the same device the UI
//!    draws with. Nothing in cxx-qt can hand a foreign texture to a
//!    `QQuickItem`; doing it properly needs a `QQuickRhiItem` subclass
//!    in C++ importing the Vulkan image, which is a real project and not
//!    a spike. So the frame is read back to the CPU and written as a BMP
//!    into a temp file that QML's `Image` reloads. BMP, not PNG: no
//!    compression pass, and this happens on every slider drag —
//!    measured at ~4 ms for a 1280x960 frame against ~90 ms for PNG.
//!    The readback itself is ~3 ms. See the findings in doc 07: this is
//!    the one place where the Qt port is meaningfully worse, and it is
//!    fixable, in C++.

use std::path::{Path, PathBuf};

/// Identical cap to the egui build: a source bigger than this is
/// downsized before the shader sees it, so a phone photo isn't treated
/// as a native game resolution.
const MAX_SOURCE_W: u32 = 1600;
const MAX_SOURCE_H: u32 = 1200;

pub struct Preview {
    device: wgpu::Device,
    queue: wgpu::Queue,
    adapter_info: wgpu::AdapterInfo,
    image_path: Option<PathBuf>,
    input: Option<(wgpu::Texture, u32, u32)>,
    preset_path: Option<PathBuf>,
    chain: Option<shader_chain::Chain>,
    /// The size the last frame actually came out at — the input image's
    /// own size times the largest integer factor that fits the area,
    /// exactly as `player::Gpu::viewport` computes it. QML centres a
    /// frame this size on black, so the preview letterboxes the way the
    /// player's window does rather than stretching.
    viewport: (u32, u32),
    error: Option<String>,
    /// The file QML's `Image` reads, and a counter appended to its URL as
    /// a query string — QML caches by URL, so the same path with the same
    /// query would never be re-read.
    out_path: PathBuf,
    generation: u64,
}

impl Preview {
    /// Opens a windowless adapter and device. Fails only if there is no
    /// usable GPU at all, which is the same condition that would stop the
    /// player from running, so it is reported rather than worked around.
    pub fn new() -> Preview {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            ..Default::default()
        }))
        .expect("no suitable GPU adapter");
        let adapter_info = adapter.get_info();
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("shader preview"),
            ..Default::default()
        }))
        .expect("request device");
        let out_path = std::env::temp_dir().join(format!("2ksbox-preview-{}.bmp", std::process::id()));
        Preview {
            device,
            queue,
            adapter_info,
            image_path: None,
            input: None,
            preset_path: None,
            chain: None,
            viewport: (0, 0),
            error: None,
            out_path,
            generation: 0,
        }
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    pub fn viewport(&self) -> (u32, u32) {
        self.viewport
    }

    /// The URL for QML's `Image.source`: empty until a frame exists, and
    /// carrying a generation counter so each new frame is a new URL.
    pub fn source_url(&self) -> String {
        if self.generation == 0 {
            return String::new();
        }
        format!("file://{}?v={}", self.out_path.display(), self.generation)
    }

    /// Reflect the editor's current preset, parameter values, image and
    /// available area — reloading only what changed and re-rendering.
    /// Same signature and same order of work as the egui build's
    /// `Preview::update`, with the area in pixels rather than points.
    pub fn update(&mut self, preset: &Path, params: &[(String, f32)], image: &Path, area_w: u32, area_h: u32) {
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
        self.render(area_w, area_h);
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
        let img = if img.width() > MAX_SOURCE_W || img.height() > MAX_SOURCE_H {
            img.resize(MAX_SOURCE_W, MAX_SOURCE_H, image::imageops::FilterType::Triangle)
        } else {
            img
        };
        let img = img.to_rgba8();
        let (w, h) = img.dimensions();
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("shader preview input"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            // Not sRGB, same as the egui build and the player: the
            // preset's own GAMMA_INPUT parameter does that math.
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        self.queue.write_texture(
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
        match shader_chain::Chain::load(
            path,
            &self.device,
            &self.queue,
            self.adapter_info.clone(),
            wgpu::TextureFormat::Rgba8Unorm,
        ) {
            Ok(c) => self.chain = Some(c),
            Err(e) => self.error = Some(format!("loading shader: {e}")),
        }
    }

    fn render(&mut self, area_w: u32, area_h: u32) {
        let Some((_, iw, ih)) = &self.input else { return };
        let (iw, ih) = (*iw, *ih);
        if self.chain.is_none() {
            return;
        }
        // `player::Gpu::viewport`'s math, unchanged: the largest integer
        // scale that fits, floored, never below 1 (a fractional scale
        // would blur the very thing being previewed, and some presets
        // divide by `floor(OutputSize/SourceSize)`).
        let (aw, ah) = (area_w.max(1) as f32, area_h.max(1) as f32);
        let scale = (aw / iw as f32).min(ah / ih as f32).floor().max(1.0);
        let (rw, rh) = ((iw as f32 * scale) as u32, (ih as f32 * scale) as u32);
        self.viewport = (rw, rh);

        let mut encoder =
            self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shader preview") });
        let result = {
            let input_tex = &self.input.as_ref().unwrap().0;
            let chain = self.chain.as_mut().unwrap();
            chain.run(&self.device, &mut encoder, input_tex, rw, rh).map(|_| ())
        };
        match result {
            Ok(()) => {
                self.queue.submit(Some(encoder.finish()));
                if let Err(e) = self.write_frame() {
                    self.error = Some(e);
                }
            }
            Err(e) => self.error = Some(e),
        }
    }

    /// Read the chain's output back and drop it on disk as a BMP for
    /// QML. The whole cost the egui build doesn't pay — see this
    /// module's header.
    ///
    /// The readback itself is `shader_chain::read_texture`, the same one
    /// the player's mode sweep and both launchers' `--preview-shader`
    /// dumps use, so the row-stride and BGRA handling live in one place.
    fn write_frame(&mut self) -> Result<(), String> {
        let Some(tex) = self.chain.as_ref().and_then(shader_chain::Chain::output_texture) else {
            return Err("no frame rendered".into());
        };
        let (w, h, rgb) = shader_chain::read_texture(&self.device, &self.queue, tex);
        let buf = image::RgbImage::from_raw(w, h, rgb).ok_or("readback size mismatch")?;
        // A temp file, then rename: QML's `Image` can otherwise pick up a
        // half-written frame and render a torn one.
        let tmp = self.out_path.with_extension("bmp.tmp");
        buf.save_with_format(&tmp, image::ImageFormat::Bmp)
            .map_err(|e| format!("writing the preview frame: {e}"))?;
        std::fs::rename(&tmp, &self.out_path).map_err(|e| format!("{}: {e}", self.out_path.display()))?;
        self.generation += 1;
        Ok(())
    }

    /// The `--preview-shader` verb's output: the same PNG dump the egui
    /// build's verb writes, through the same `shader-chain` helper, so
    /// the two builds' frames can be compared byte for byte.
    pub fn dump_png(&self, out: &str) -> Result<(), String> {
        let tex = self.chain.as_ref().and_then(shader_chain::Chain::output_texture).ok_or("no frame rendered")?;
        shader_chain::dump_texture(&self.device, &self.queue, tex, out);
        Ok(())
    }
}
