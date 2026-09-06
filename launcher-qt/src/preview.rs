//! The shader profile editor's live preview, on Qt.
//!
//! The render itself is `launcher_core::preview::Preview` — the same
//! decode, the same `shader-chain`, the same "integer scale, then
//! letterbox" viewport math as `player::Gpu::viewport` — shared with the
//! egui build rather than written twice. What is here is the two things
//! Qt makes different, and they are the interesting part of the whole
//! port:
//!
//! 1. **Whose GPU.** eframe hands egui a live `wgpu::Device`/`Queue` and
//!    the egui build's preview borrows it. Qt Quick renders through its
//!    own abstraction (QRhi) on Vulkan, and cxx-qt exposes no handle to
//!    it, so this calls `Preview::headless` and gets a *second*,
//!    windowless wgpu device. On this box that is a second Vulkan
//!    logical device on the same physical GPU: about 40 MB of extra VRAM
//!    and one more driver context, invisible in use, but a real cost the
//!    egui build does not pay.
//!
//! 2. **How the frame reaches the widget.** egui takes the rendered
//!    texture by id — zero copy, it is already on the device the UI
//!    draws with. Nothing in cxx-qt can hand a foreign texture to a
//!    `QQuickItem`; doing it properly needs a `QQuickRhiItem` subclass
//!    in C++ importing the Vulkan image, which is a real project. So the
//!    frame is read back to the CPU (`Preview::read_frame`, which is
//!    `shader_chain::read_texture` under it, so row strides and BGRA
//!    handling stay in one place) and written as a BMP into a temp file
//!    that QML's `Image` reloads. BMP, not PNG: no compression pass, and
//!    this happens on every slider drag — measured at ~4 ms for a
//!    1280x960 frame against ~90 ms for PNG. The readback itself is
//!    ~3 ms. See doc 07: this is the one place the Qt build is
//!    meaningfully worse, and it is fixable, in C++.

use launcher_core::preview::Preview as Core;
use std::path::{Path, PathBuf};

pub struct Preview {
    core: Core,
    /// The file QML's `Image` reads, and a counter appended to its URL as
    /// a query string — QML caches by URL, so the same path with the same
    /// query would never be re-read.
    out_path: PathBuf,
    generation: u64,
    /// A readback or write failure, which the core render knows nothing
    /// about.
    write_error: Option<String>,
}

impl Preview {
    /// Fails only if there is no usable GPU at all, which is the same
    /// condition that would stop the player from running.
    pub fn new() -> Preview {
        let core = Core::headless().expect("a windowless wgpu device for the preview");
        let out_path = std::env::temp_dir().join(format!("2ksbox-preview-{}.bmp", std::process::id()));
        Preview { core, out_path, generation: 0, write_error: None }
    }

    pub fn error(&self) -> Option<&str> {
        self.write_error.as_deref().or_else(|| self.core.error())
    }

    pub fn viewport(&self) -> (u32, u32) {
        self.core.viewport()
    }

    /// The URL for QML's `Image.source`: empty until a frame exists, and
    /// carrying a generation counter so each new frame is a new URL.
    pub fn source_url(&self) -> String {
        if self.generation == 0 {
            return String::new();
        }
        format!("file://{}?v={}", self.out_path.display(), self.generation)
    }

    /// Render, then put the frame where QML can read it.
    pub fn update(&mut self, preset: &Path, params: &[(String, f32)], image: &Path, area_w: u32, area_h: u32) {
        self.write_error = None;
        self.core.update(preset, params, image, area_w, area_h);
        if self.core.error().is_some() {
            return;
        }
        if let Err(e) = self.write_frame() {
            self.write_error = Some(e);
        }
    }

    fn write_frame(&mut self) -> Result<(), String> {
        let (w, h, rgb) = self.core.read_frame().ok_or("no frame rendered")?;
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
}
