//! The shader profile editor's live preview: a still image (a game
//! screenshot, typically) run through the same `shader-chain` crate the
//! player uses, re-rendered whenever the preset, its parameter values or
//! the image change.
//!
//! **Whose GPU is the caller's business.** eframe already has a
//! `wgpu::Device`/`Queue` open to draw egui with, and the egui build
//! hands them here so the rendered texture reaches the widget by id —
//! zero copy. Qt Quick renders through QRhi and cxx-qt exposes no handle
//! to it, so the Qt build calls `headless()` and gets a second,
//! windowless device of its own (~40 MB of VRAM and one more driver
//! context) and reads the frame back to the CPU. Everything between
//! those two ends — decoding the image, the source-size cap, loading the
//! chain, the integer-scale viewport math — is this file, once.
//!
//! `output_texture()` is where the two paths diverge again: the egui
//! build registers it with `egui_wgpu`, the Qt build runs
//! `shader_chain::read_texture` over it and writes a BMP, and the
//! `--preview-shader` verb dumps it as a PNG. That the two front ends'
//! verbs produce byte-identical PNGs is the check that this really is
//! one render path (doc 07).

use std::path::{Path, PathBuf};

/// A source bigger than this is downsized on the CPU before the shader
/// ever sees it. Generous enough to pass any real game resolution
/// through untouched — this is meant to *look like the player*: a native
/// game resolution treated as-is, then integer-scaled up exactly the way
/// `player::Gpu::viewport` does — it is only a sanity cap against
/// rendering, say, a 12-megapixel phone photo at full size every frame.
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
    viewport: (u32, u32),
    error: Option<String>,
}

impl Preview {
    /// Render on a device the caller already has — eframe's, for the
    /// egui build, so the preview costs no second GPU context.
    pub fn new(device: wgpu::Device, queue: wgpu::Queue, adapter_info: wgpu::AdapterInfo) -> Preview {
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
        }
    }

    /// Open a windowless adapter and device of this preview's own — for
    /// a front end whose toolkit will not lend one, and for the headless
    /// verbs. Fails only when there is no usable GPU at all, which is
    /// the same condition that would stop the player from running.
    pub fn headless() -> Result<Preview, String> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            ..Default::default()
        }))
        .map_err(|e| format!("no suitable GPU adapter: {e}"))?;
        let adapter_info = adapter.get_info();
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("shader preview"),
            ..Default::default()
        }))
        .map_err(|e| format!("requesting a device: {e}"))?;
        Ok(Preview::new(device, queue, adapter_info))
    }

    pub fn device(&self) -> &wgpu::Device {
        &self.device
    }

    pub fn queue(&self) -> &wgpu::Queue {
        &self.queue
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// The exact size the last rendered frame came out at: the input
    /// image's own size times the largest *integer* factor that fits the
    /// area asked for. Never a fraction, so the shader is never blurred
    /// by a second, non-integer resample the way stretching it to fill
    /// the area exactly would; smaller than the area on one axis
    /// (letterboxed) unless the aspect ratios happen to match. The
    /// caller centres it and fills the rest with black — "how it will
    /// really look in the player", not an image widget stretched to fit.
    pub fn viewport(&self) -> (u32, u32) {
        self.viewport
    }

    /// The last rendered frame, for whatever the caller does with it:
    /// register it as an egui texture, read it back to a file, dump it.
    pub fn output_texture(&self) -> Option<&wgpu::Texture> {
        self.chain.as_ref().and_then(shader_chain::Chain::output_texture)
    }

    /// The view onto it — what a toolkit that takes a texture by handle
    /// wants (`egui_wgpu::Renderer::register_native_texture`).
    pub fn output_view(&self) -> Option<&wgpu::TextureView> {
        self.chain.as_ref().and_then(shader_chain::Chain::output_view)
    }

    /// Reflect the editor's current preset path, effective parameter
    /// values (defaults already merged with overrides — this doesn't
    /// need to know which is which), image path, and the area available
    /// to render into, reloading only what actually changed and
    /// re-rendering a frame.
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

    /// Read the last frame back as `(width, height, RGB8)` — the CPU
    /// path, for a front end that cannot take a texture.
    pub fn read_frame(&self) -> Option<(u32, u32, Vec<u8>)> {
        let tex = self.output_texture()?;
        Some(shader_chain::read_texture(&self.device, &self.queue, tex))
    }

    /// Dump the last frame as a PNG — what both `--preview-shader` verbs
    /// write, so the two front ends' output can be diffed byte for byte.
    pub fn dump_png(&self, out: &str) -> Result<(), String> {
        let tex = self.output_texture().ok_or("no frame rendered")?;
        shader_chain::dump_texture(&self.device, &self.queue, tex, out);
        Ok(())
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
        // can't happen regardless of this cap — this is just the sanity
        // limit against treating an arbitrarily huge photo as "native
        // resolution" and rendering it at full size every frame.
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
            // Not sRGB: the shader's own GAMMA_INPUT parameter (present
            // on most CRT presets) does that math itself, same as the
            // player feeding it raw (non-sRGB-tagged) guest pixels.
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
        // `egui_wgpu::Renderer::register_native_texture` requires exactly
        // this format for a texture it is handed, and the readback path
        // is indifferent, so both ends agree on it.
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
        // Exactly `player::Gpu::viewport`'s own math: the largest
        // *integer* scale that fits the area, floored (never a fraction
        // — that would blur the very thing being previewed) and never
        // below 1 (never shrunk; this is also what keeps some presets'
        // own `1.0 / floor(OutputSize/SourceSize)` away from a division
        // by zero). Below 1 in the area itself, the rendered frame is
        // bigger than the area and the caller crops it around the
        // centre — same as making the real player's window smaller than
        // the guest's native resolution.
        let (aw, ah) = (area_w.max(1) as f32, area_h.max(1) as f32);
        let scale = (aw / iw as f32).min(ah / ih as f32).floor().max(1.0);
        let (rw, rh) = ((iw as f32 * scale) as u32, (ih as f32 * scale) as u32);
        self.viewport = (rw, rh);

        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shader preview") });
        let result = {
            let input_tex = &self.input.as_ref().unwrap().0;
            let chain = self.chain.as_mut().unwrap();
            chain.run(&self.device, &mut encoder, input_tex, rw, rh).map(|_| ())
        };
        match result {
            Ok(()) => {
                self.queue.submit(Some(encoder.finish()));
            }
            Err(e) => self.error = Some(e),
        }
    }
}
