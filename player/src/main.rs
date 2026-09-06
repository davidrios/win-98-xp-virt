//! win98-xp-virt player (doc 02): one running machine per process.
//!
//! M1 state: `player -- <qemu-system args>` boots QEMU in-process and
//! presents the guest framebuffer through wgpu (integer-scaled 4:3);
//! keyboard and mouse are injected. No args → the M0 test pattern.

mod audio;
#[cfg(target_os = "linux")]
mod dmabuf;
#[cfg(target_os = "macos")]
mod iosurface;
mod keymap;
mod mode;
mod pattern;
mod qemu_vm;
mod qmp;
mod shader;

use pattern::Pattern;
use qemu_embed::Qemu;
use std::sync::Arc;
use winit::application::ApplicationHandler;
use winit::dpi::LogicalSize;
use winit::event::{
    DeviceEvent, DeviceId, ElementState, MouseButton, MouseScrollDelta, WindowEvent,
};
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};
use winit::keyboard::{KeyCode, ModifiersState, PhysicalKey};
use winit::window::{Cursor, CursorGrabMode, CustomCursor, Window, WindowId};

struct Gpu {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    bgl: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    pipeline: wgpu::RenderPipeline,
    fb_tex: Option<(wgpu::Texture, wgpu::BindGroup, u32, u32)>,
    /// zero-copy 3D frames: imported dma-buf ring slots and the one on show
    ext: Vec<Option<(wgpu::Texture, wgpu::BindGroup, u32, u32)>>,
    ext_current: Option<usize>,
    zero_copy: bool,
    adapter_info: wgpu::AdapterInfo,
    chain: Option<shader::Chain>,
    chain_bg: Option<(wgpu::BindGroup, u32, u32)>,
    /// mode analysis of the guest surface on show (doc 03 rules 2 and 3)
    mode: mode::Mode,
    /// the loaded preset has no parameter to carry a scanline count: said once
    warned_no_scanline_params: bool,
    /// the mode sweep renders to a fixed surface size instead of the window's,
    /// so what it checks does not depend on what the compositor handed us
    forced_surface: Option<(u32, u32)>,
}

impl Gpu {
    fn new(window: Arc<Window>) -> Self {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let surface = instance
            .create_surface(window.clone())
            .expect("create surface");
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            ..Default::default()
        }))
        .expect("no suitable GPU adapter");
        let desc = wgpu::DeviceDescriptor {
            label: Some("player"),
            ..Default::default()
        };
        // Linux: open the device with the dma-buf import extensions so 3D
        // frames can be sampled straight from the backend's buffers.
        #[cfg(target_os = "linux")]
        let opened = dmabuf::create_device(&adapter, &desc);
        #[cfg(not(target_os = "linux"))]
        let opened: Option<(wgpu::Device, wgpu::Queue, bool)> = None;
        let (device, queue, zero_copy) = match opened {
            Some(t) => t,
            None => {
                let (d, q) = pollster::block_on(adapter.request_device(&desc)).expect("request device");
                // macOS: IOSurface-backed Metal textures need no extensions
                (d, q, cfg!(target_os = "macos"))
            }
        };
        if zero_copy {
            eprintln!("[3d] zero-copy dma-buf import available");
        }

        let size = window.inner_size();
        let mut config = surface
            .get_default_config(&adapter, size.width.max(1), size.height.max(1))
            .expect("surface unsupported by adapter");
        let caps = surface.get_capabilities(&adapter);
        if caps.present_modes.contains(&wgpu::PresentMode::Mailbox) {
            config.present_mode = wgpu::PresentMode::Mailbox;
        }
        config.desired_maximum_frame_latency = 1;
        surface.configure(&device, &config);

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("nearest"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("blit bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("blit"),
            source: wgpu::ShaderSource::Wgsl(include_str!("blit.wgsl").into()),
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("blit layout"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("blit"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: config.format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });

        let adapter_info = adapter.get_info();
        Self {
            window,
            surface,
            device,
            queue,
            config,
            bgl,
            sampler,
            pipeline,
            fb_tex: None,
            ext: Vec::new(),
            ext_current: None,
            zero_copy,
            adapter_info,
            chain: None,
            chain_bg: None,
            mode: mode::Mode::analyse(0, 0),
            warned_no_scanline_params: false,
            forced_surface: None,
        }
    }

    fn load_shader(&mut self, path: &std::path::Path) {
        match shader::Chain::load(
            path,
            &self.device,
            &self.queue,
            self.adapter_info.clone(),
            self.config.format,
        ) {
            Ok(c) => {
                eprintln!("[shader] loaded {}", path.display());
                self.chain = Some(c);
            }
            Err(e) => eprintln!("[shader] failed to load {}: {e}", path.display()),
        }
    }

    fn resize(&mut self, w: u32, h: u32) {
        self.config.width = w.max(1);
        self.config.height = h.max(1);
        self.surface.configure(&self.device, &self.config);
    }

    /// (Re)create the guest framebuffer texture when its size changes.
    fn ensure_texture(&mut self, w: u32, h: u32) {
        if matches!(&self.fb_tex, Some((_, _, tw, th)) if *tw == w && *th == h) {
            return;
        }
        // XRGB8888 little-endian == BGRA8 byte order: upload as-is.
        // Guest pixels are sRGB-encoded: tag the texture sRGB when the swapchain
        // is sRGB (macOS default) so sampling decodes and presenting re-encodes;
        // on a linear swapchain (Linux default) pass values through unchanged.
        let format = if self.config.format.is_srgb() {
            wgpu::TextureFormat::Bgra8UnormSrgb
        } else {
            wgpu::TextureFormat::Bgra8Unorm
        };
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("guest framebuffer"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let bg = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("blit bg"),
            layout: &self.bgl,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
            ],
        });
        self.fb_tex = Some((tex, bg, w, h));
    }

    fn make_bind_group(&self, tex: &wgpu::Texture) -> wgpu::BindGroup {
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("blit bg"),
            layout: &self.bgl,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
            ],
        })
    }

    /// Import a backend ring slot: a dma-buf (Linux, takes the fd) or an
    /// IOSurface (macOS).
    fn import_slot(&mut self, d: &qemu_vm::DmaBuf) {
        let srgb = self.config.format.is_srgb();
        let (slot, w, h) = (d.slot, d.w, d.h);
        #[cfg(target_os = "linux")]
        let r = dmabuf::import(&self.device, d.fd, w, h, d.stride, d.fourcc, d.modifier, srgb);
        #[cfg(target_os = "macos")]
        let r = iosurface::import(&self.device, d.iosurface as *mut std::ffi::c_void, w, h, srgb);
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        let r: Result<wgpu::Texture, String> = {
            if d.fd >= 0 {
                unsafe { libc::close(d.fd) };
            }
            Err("no zero-copy import on this platform".into())
        };
        match r {
            Ok(tex) => {
                let bg = self.make_bind_group(&tex);
                if self.ext.len() <= slot {
                    self.ext.resize_with(slot + 1, || None);
                }
                self.ext[slot] = Some((tex, bg, w, h));
                eprintln!("[3d] slot {slot}: imported {w}x{h}");
            }
            Err(e) => {
                eprintln!("[3d] slot {slot}: zero-copy import failed: {e}");
                if self.ext.len() > slot {
                    self.ext[slot] = None;
                }
            }
        }
    }

    /// Show an imported slot (Some) or the CPU-uploaded framebuffer (None).
    fn use_slot(&mut self, slot: Option<usize>) {
        self.ext_current = match slot {
            Some(s) if self.ext.get(s).map(|e| e.is_some()).unwrap_or(false) => Some(s),
            _ => None,
        };
    }

    /// The texture currently on show: an imported 3D slot or the upload.
    fn current(&self) -> Option<&(wgpu::Texture, wgpu::BindGroup, u32, u32)> {
        match self.ext_current {
            Some(s) => self.ext[s].as_ref(),
            None => self.fb_tex.as_ref(),
        }
    }

    fn upload(&mut self, pixels: &[u32], w: u32, h: u32) {
        self.ensure_texture(w, h);
        let (tex, _, _, _) = self.fb_tex.as_ref().unwrap();
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(pixels),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(w * 4),
                rows_per_image: Some(h),
            },
            wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
        );
    }

    /// Largest rect of the mode's own display aspect that fits the surface,
    /// centered (doc 03 geometry stage, rules 2 and 4).
    ///
    /// The height is an integer multiple of the guest's rows so scanlines
    /// stay even, and the width then follows the display aspect rather than
    /// the framebuffer's ratio — which is the whole point of rule 2: a
    /// 320x200 mode is a 4:3 picture, not a 1.6:1 one, and integer-scaling
    /// both axes would show it stretched. Square-pixel 4:3 modes (640x480,
    /// 800x600, …) come out exactly as they did before.
    fn viewport(&self) -> (f32, f32, f32, f32) {
        let Some((_, _, tw, th)) = self.current() else {
            return (0.0, 0.0, 1.0, 1.0);
        };
        let (tw, th) = (*tw, *th);
        let (sw, sh) = self.surface_size();
        let (sw, sh) = (sw as f32, sh as f32);
        let m = mode::Mode::analyse(tw, th);
        let dar = m.display_aspect;
        // The vertical quantum is the scanline, not the guest row: on a
        // double-scanned mode they differ, and it is the scanline pitch that
        // has to come out even — 320x200 in a 2400-line surface is 6 pixels
        // per scanline this way and 5.5 if the rows are quantised instead.
        // Every whole scale of the scanlines is a whole scale of the rows
        // too, so this only ever refines the old rule.
        let gh = m.scanlines as f32;
        // bounded by both axes once the width is corrected; when even 1x does
        // not fit, fall back to a free fit so the picture is letterboxed
        // rather than clipped
        let scale = (sh / gh).floor().min((sw / (gh * dar)).floor());
        let (vw, vh) = if scale >= 1.0 {
            (gh * scale * dar, gh * scale)
        } else if sw / sh > dar {
            (sh * dar, sh)
        } else {
            (sw, sw / dar)
        };
        ((sw - vw) / 2.0, (sh - vh) / 2.0, vw, vh)
    }

    /// The surface the geometry stage fits the picture into.
    fn surface_size(&self) -> (u32, u32) {
        self.forced_surface
            .unwrap_or((self.config.width, self.config.height))
    }

    /// Re-analyse when the guest changes mode: log what the surface means
    /// and tell the CRT preset this mode's scanline count (doc 03 rule 3).
    fn update_mode(&mut self) {
        let Some((_, _, tw, th)) = self.current() else {
            return;
        };
        let (tw, th) = (*tw, *th);
        if self.mode.width == tw && self.mode.height == th {
            return;
        }
        self.mode = mode::Mode::analyse(tw, th);
        eprintln!("[display] mode {}", self.mode.describe());
        let params = self.mode.shader_params();
        let Some(chain) = self.chain.as_ref() else {
            return;
        };
        // the A/B control: the preset left to its own resolution guess, which
        // is what every scanline preset did before mode analysis existed
        if std::env::var("PLAYER_MODE_PARAMS").as_deref() == Ok("0") {
            eprintln!("[shader] PLAYER_MODE_PARAMS=0: preset left to guess the scanline count");
            return;
        }
        if params.iter().all(|(n, _)| chain.has_parameter(n)) {
            chain.set_parameters(&params);
            let set: Vec<String> = params.iter().map(|(n, v)| format!("{n}={v}")).collect();
            eprintln!("[shader] mode parameters {}", set.join(" "));
        } else if !self.warned_no_scanline_params {
            self.warned_no_scanline_params = true;
            eprintln!(
                "[shader] this preset exposes no scanline-count parameter \
                 ({}): a double-scanned mode will be drawn with one scanline \
                 per guest row instead of the two per row the tube drew",
                params
                    .iter()
                    .map(|(n, _)| n.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
            );
        }
    }

    /// Next swapchain image. With FIFO this blocks until one is free; call
    /// it BEFORE sampling the guest frame so the newest frame is presented
    /// (a saturated queue otherwise ages every frame by a host vblank).
    fn acquire(&mut self) -> Option<wgpu::SurfaceTexture> {
        use wgpu::CurrentSurfaceTexture as Cst;
        match self.surface.get_current_texture() {
            Cst::Success(f) | Cst::Suboptimal(f) => Some(f),
            Cst::Timeout | Cst::Occluded => None,
            _ => {
                self.resize(self.config.width, self.config.height);
                None
            }
        }
    }

    /// Run the shader chain and, if `frame` is given, blit and present it.
    fn render(&mut self, frame: Option<wgpu::SurfaceTexture>) {
        if self.current().is_none() {
            return;
        }
        self.update_mode();
        // CRT chain: guest texture → viewport-sized output texture (doc 03)
        let (_, _, vw, vh) = self.viewport();
        let (vw, vh) = (vw.max(1.0) as u32, vh.max(1.0) as u32);
        let mut chain_enc = None;
        if let Some(chain) = self.chain.as_mut() {
            let mut enc = self
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                    label: Some("shader"),
                });
            // field-level borrows: `chain` is borrowed mutably above
            let input: &wgpu::Texture = match self.ext_current {
                Some(s) => &self.ext[s].as_ref().unwrap().0,
                None => &self.fb_tex.as_ref().unwrap().0,
            };
            match chain.run(&self.device, &mut enc, input, vw, vh) {
                Ok(out) => {
                    let out_tex = out.clone();
                    if !matches!(&self.chain_bg, Some((_, w, h)) if *w == vw && *h == vh) {
                        let view = out_tex.create_view(&wgpu::TextureViewDescriptor::default());
                        let bg = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
                            label: Some("chain bg"),
                            layout: &self.bgl,
                            entries: &[
                                wgpu::BindGroupEntry {
                                    binding: 0,
                                    resource: wgpu::BindingResource::TextureView(&view),
                                },
                                wgpu::BindGroupEntry {
                                    binding: 1,
                                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                                },
                            ],
                        });
                        self.chain_bg = Some((bg, vw, vh));
                    }
                    chain_enc = Some(enc);
                    if let Ok(path) = std::env::var("PLAYER_DUMP_OUT") {
                        let want: usize = std::env::var("PLAYER_DUMP_SEQ")
                            .ok()
                            .and_then(|v| v.parse().ok())
                            .unwrap_or(60);
                        if chain.frame_count() == want {
                            self.queue.submit(Some(chain_enc.take().unwrap().finish()));
                            shader::dump_texture(&self.device, &self.queue, &out_tex, &path);
                            hard_exit(0);
                        }
                    }
                }
                Err(e) => {
                    eprintln!("[shader] frame failed: {e}; disabling chain");
                    self.chain = None;
                    self.chain_bg = None;
                }
            }
        }
        if let Some(enc) = chain_enc {
            self.queue.submit(Some(enc.finish()));
        }
        let bg: &wgpu::BindGroup = match (&self.chain, &self.chain_bg) {
            (Some(_), Some((bg, _, _))) => bg,
            _ => &self.current().unwrap().1,
        };
        let Some(frame) = frame else { return };
        let view = frame
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("frame"),
            });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("blit"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            let (x, y, w, h) = self.viewport();
            pass.set_viewport(x, y, w, h, 0.0, 1.0);
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, bg, &[]);
            pass.draw(0..3, 0..1);
        }
        self.queue.submit(Some(encoder.finish()));
        self.window.pre_present_notify();
        self.queue.present(frame);
    }
}

enum Source {
    Pattern(Pattern),
    Sweep(Sweep),
    Calib(Calib),
    Qemu {
        vm: Qemu,
        display: qemu_vm::Display,
        last_seq: u64,
        qmp: Option<Arc<qmp::Qmp>>,
        /// PLAYER_QMP_EXEC ran (once, after the first guest frame)
        qmp_exec_done: bool,
    },
}

#[derive(Default)]
struct App {
    qemu_args: Vec<String>,
    shader: Option<std::path::PathBuf>,
    gpu: Option<Gpu>,
    source: Option<Source>,
    audio: Option<audio::Output>,
    latency: Vec<f32>, // ms, publish→present per presented guest frame
    modifiers: ModifiersState,
    grabbed: bool,
    /// Keys currently held in the guest (QEMU qcodes). Lifted when the window
    /// loses focus: a host shortcut (Cmd+Tab on macOS) delivers the modifier's
    /// press to us and its release to the app that took over, and the guest
    /// would otherwise keep the Windows key down forever.
    keys_down: Vec<u32>,
    /// Set on CloseRequested: no further calls into the VM handle, which the
    /// QEMU thread is about to destroy.
    closing: bool,
    qemu_thread: Option<std::thread::JoinHandle<i32>>,
    /// Wakes the event loop from the QEMU thread when a frame is published.
    proxy: Option<EventLoopProxy<()>>,
    /// The guest's hardware cursor (the d3dpt-vga driver, doc 15) as a host
    /// cursor: shown with the guest's shape while the pointer is over the
    /// image and the guest shows it, hidden when the guest hides it. With
    /// the USB tablet the host pointer is where the guest cursor is, so
    /// nothing is composited. A guest without one (a software pointer in
    /// the framebuffer) keeps the host cursor hidden over the image.
    guest_cursor: Option<CustomCursor>,
    guest_cursor_seq: u64,
    /// The pointer is over the image (CursorMoved inside the viewport).
    pointer_inside: bool,
    /// What the window's cursor was last set to (wakes come every frame).
    cursor_applied: HostCursor,
    /// `--mode-sweep <dir>`: run the mode sweep instead of a guest.
    sweep: Option<std::path::PathBuf>,
    /// `--calib <bmp|dir>`: shade the calibration patterns and exit.
    calib: Option<std::path::PathBuf>,
}

/// The host window's cursor state the player last applied.
#[derive(Default, PartialEq, Clone, Copy)]
enum HostCursor {
    #[default]
    Default,
    Hidden,
    /// the guest's shape of this sequence number
    Guest(u64),
}

/// Shade the calibration patterns (doc 09): each BMP that
/// `tools/crtcal-render` wrote goes through the loaded preset at the size it
/// was drawn at, and the shaded frame lands beside it as a PNG. That is the
/// other half of the comparison — one photograph of the tube showing the
/// pattern, one shaded frame of the same pattern, held side by side.
struct Calib {
    files: Vec<std::path::PathBuf>,
    i: usize,
    last_surface: (u32, u32),
}

/// The 24-bit bottom-up BMPs `crtcal-render` writes. Deliberately not a
/// general decoder: anything else is a mistake worth reporting, not
/// something to guess at.
fn read_bmp(path: &std::path::Path) -> Result<(u32, u32, Vec<u32>), String> {
    let d = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if d.len() < 54 || d[0] != b'B' || d[1] != b'M' {
        return Err(format!("{}: not a BMP", path.display()));
    }
    let u32le = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]);
    let (off, w, h) = (u32le(10) as usize, u32le(18), u32le(22));
    let bpp = u16::from_le_bytes([d[28], d[29]]);
    if bpp != 24 {
        return Err(format!("{}: {bpp}-bit BMP, expected 24", path.display()));
    }
    let stride = (w as usize * 3 + 3) & !3;
    if off + stride * h as usize > d.len() {
        return Err(format!("{}: truncated", path.display()));
    }
    let mut px = vec![0u32; (w * h) as usize];
    for y in 0..h as usize {
        let src = off + (h as usize - 1 - y) * stride; // BMP rows run bottom-up
        for x in 0..w as usize {
            let p = &d[src + x * 3..src + x * 3 + 3];
            px[y * w as usize + x] =
                (p[2] as u32) << 16 | (p[1] as u32) << 8 | p[0] as u32;
        }
    }
    Ok((w, h, px))
}

/// The surface the sweep fits its pictures into: 1600x1200, the tallest mode
/// in the table, needs 2400 lines for its 1200 scanlines to be countable.
const SWEEP_SURFACE: (u32, u32) = (3200, 2400);

/// The mode sweep (doc 03's "mode-sweep test", M2): step through every mode
/// the table knows, upload a geometry pattern at that size and run the real
/// display path — mode analysis, the geometry stage, the loaded preset —
/// then check what each did with it. No guest and no QEMU: the boundary
/// under test is the player's own display path.
struct Sweep {
    out: std::path::PathBuf,
    sizes: Vec<(u32, u32)>,
    i: usize,
    fails: Vec<String>,
    /// a preset was asked for on the command line
    want_chain: bool,
    /// surface size at the last redraw: the compositor settles on one a
    /// frame or two in, and a mode measured against a size that is about to
    /// change is measured against nothing
    last_surface: (u32, u32),
}

impl Sweep {
    fn new(out: std::path::PathBuf, want_chain: bool) -> Sweep {
        Sweep {
            out,
            sizes: mode::sweep_sizes(),
            i: 0,
            fails: Vec::new(),
            want_chain,
            last_surface: (0, 0),
        }
    }
}

/// Upload the geometry pattern for the mode about to be checked.
fn sweep_upload(gpu: &mut Gpu, s: &Sweep) {
    let (w, h) = s.sizes[s.i];
    let m = mode::Mode::analyse(w, h);
    let fb = pattern::geometry(w as usize, h as usize, m.display_aspect);
    gpu.upload(&fb, w, h);
}

/// The number of scanlines a shaded frame actually has: count the bright
/// bands down one column of flat picture (left edge, below the line-pair
/// block and clear of the circle) and scale to the full height.
///
/// Counted rather than measured as a repeat period, because the pitch need
/// not be a whole number of output pixels — 400 scanlines in a 2200-pixel
/// viewport alternate 5 and 6 pixels, and their *period* is then two
/// scanlines, which would read as half the count.
fn measure_scanlines(w: u32, h: u32, rgb: &[u8]) -> Option<u32> {
    let x = (w as f32 * 0.08) as u32;
    let (y0, y1) = (h * 2 / 5, h * 7 / 10);
    let lum: Vec<f32> = (y0..y1)
        .map(|y| {
            let i = ((y * w + x) * 3) as usize;
            0.299 * rgb[i] as f32 + 0.587 * rgb[i + 1] as f32 + 0.114 * rgb[i + 2] as f32
        })
        .collect();
    let (lo, hi) = lum.iter().fold((f32::MAX, f32::MIN), |(a, b), v| (a.min(*v), b.max(*v)));
    if hi - lo < 4.0 {
        return None; // no scanline structure to count
    }
    // upward crossings of the midpoint, with hysteresis so the shader's own
    // dithering cannot add a band
    let mid = (lo + hi) / 2.0;
    let (up, down) = (mid + (hi - lo) * 0.15, mid - (hi - lo) * 0.15);
    let mut crossings: Vec<f32> = Vec::new();
    let mut armed = false;
    for (i, v) in lum.iter().enumerate() {
        if *v < down {
            armed = true;
        } else if *v > up && armed {
            // where the rise crossed the midpoint, to sub-pixel precision
            let (prev, here) = (lum[i.saturating_sub(1)], *v);
            let frac = if here > prev {
                ((mid - prev) / (here - prev)).clamp(0.0, 1.0)
            } else {
                0.0
            };
            crossings.push(i as f32 - 1.0 + frac);
            armed = false;
        }
    }
    if crossings.len() < 4 {
        return None;
    }
    // the pitch from the span between the first and last band, not from the
    // count over the sampled strip: the strip's own length would round in
    let pitch = (crossings[crossings.len() - 1] - crossings[0]) / (crossings.len() - 1) as f32;
    Some((h as f32 / pitch).round() as u32)
}

/// Check the frame just rendered, dump it, and step to the next mode.
/// Returns true when the sweep is finished.
fn sweep_step(gpu: &mut Gpu, s: &mut Sweep) -> bool {
    let (w, h) = s.sizes[s.i];
    let m = mode::Mode::analyse(w, h);
    if s.last_surface != gpu.surface_size() {
        s.last_surface = gpu.surface_size();
        return false; // measure this mode once the surface has settled
    }
    let (vx, vy, vw, vh) = gpu.viewport();
    let (sw, sh) = gpu.surface_size();
    let (sw, sh) = (sw as f32, sh as f32);
    let mut bad: Vec<String> = Vec::new();

    // rule 2: the picture on screen has the mode's display aspect, whatever
    // the framebuffer's own ratio is
    let got = vw / vh;
    if (got - m.display_aspect).abs() > m.display_aspect * 0.005 {
        bad.push(format!(
            "on-screen aspect {got:.4}, want {:.4}",
            m.display_aspect
        ));
    }
    // it is inside the window, centered
    if vw > sw + 0.5 || vh > sh + 0.5 || vx < -0.5 || vy < -0.5 {
        bad.push(format!("viewport {vw}x{vh}+{vx}+{vy} does not fit {sw}x{sh}"));
    }
    // rule 4: where a whole multiple of the scanlines fits, the height is one
    let scale = vh / m.scanlines as f32;
    if scale >= 1.0 && (scale - scale.round()).abs() > 0.001 {
        bad.push(format!("scanline pitch {scale:.4} px is not a whole number"));
    }
    // rule 3: the preset was told this mode's scanline count (skipped under
    // the PLAYER_MODE_PARAMS=0 control, where by definition it was not)
    let params = m.shader_params();
    let control = std::env::var("PLAYER_MODE_PARAMS").as_deref() == Ok("0");
    match gpu.chain.as_ref() {
        _ if control => {}
        Some(chain) if params.iter().all(|(n, _)| chain.has_parameter(n)) => {
            for (name, want) in &params {
                let got = chain.parameter(name).unwrap_or(f32::NAN);
                if (got - want).abs() > 0.001 {
                    bad.push(format!("preset parameter {name} is {got}, want {want}"));
                }
            }
        }
        Some(_) => {} // preset has no scanline control; update_mode said so
        None if s.want_chain => bad.push("the preset did not load".to_string()),
        None => {}
    }

    // rule 3, end to end: count the scanlines in the frame the preset just
    // drew and hold them against the ones the tube scanned. The dump is for
    // the eye — the circle is round when the geometry is right.
    let mut drawn = String::new();
    if let Some(tex) = gpu.chain.as_ref().and_then(|c| c.output()) {
        let (ow, oh, rgb) = shader::read_texture(&gpu.device, &gpu.queue, tex);
        // Below three output pixels per scanline there is nothing to count:
        // at two the preset has no room for a gap and draws a flat field
        // (measured — one LSB of modulation at 1152x864 and above).
        let countable = oh >= m.scanlines * 3;
        let measured = if countable {
            measure_scanlines(ow, oh, &rgb)
        } else {
            None
        };
        if let Some(got) = measured {
            drawn = format!(", {got} drawn");
        }
        if countable && !control {
            match measured {
                Some(got) if got == m.scanlines => {}
                Some(got) => bad.push(format!(
                    "the frame has {got} scanlines, the tube scanned {}",
                    m.scanlines
                )),
                None => bad.push("the frame has no scanline structure to count".to_string()),
            }
        }
        let path = s.out.join(format!("{w}x{h}.png"));
        shader::write_png(&path.to_string_lossy(), ow, oh, &rgb);
    }

    if bad.is_empty() {
        println!("  ok   {} → viewport {vw:.0}x{vh:.0}{drawn}", m.describe());
    } else {
        println!("  FAIL {} → viewport {vw:.0}x{vh:.0}{drawn}", m.describe());
        for b in &bad {
            println!("         {b}");
        }
        s.fails.push(format!("{w}x{h}: {}", bad.join("; ")));
    }

    s.i += 1;
    s.i >= s.sizes.len()
}

/// Pull the newest guest frame into the GPU: an imported dma-buf slot is
/// selected, a CPU frame is uploaded. Returns its publish time.
fn present_guest_frame(
    gpu: &mut Gpu,
    display: &qemu_vm::Display,
    last_seq: &mut u64,
    composite_cursor: bool,
) -> Option<std::time::Instant> {
    for d in display.take_dmabufs() {
        gpu.import_slot(&d);
    }
    let mut f = display.take_if_newer(*last_seq)?;
    *last_seq = f.seq;
    match f.ext_slot {
        Some(s) => gpu.use_slot(Some(s)),
        None => {
            // the guest's hardware cursor as a sprite in the frame, where the
            // host cursor cannot stand in for it (a relative mouse, a grab)
            if composite_cursor {
                if let Some((c, x, y)) = display.cursor_sprite() {
                    qemu_vm::composite_cursor(&mut f.pixels, f.width, f.height, &c, x, y);
                }
            }
            gpu.use_slot(None);
            gpu.upload(&f.pixels, f.width as u32, f.height as u32);
        }
    }
    Some(f.published)
}

/// Exit without running atexit handlers. QEMU registers several
/// (`audio_cleanup`, `qemu_run_exit_notifiers`); running them on a thread
/// other than the QEMU thread, or while that thread is still alive, is a
/// crash. stderr is unbuffered so diagnostics already printed are safe.
fn hard_exit(code: i32) -> ! {
    #[cfg(unix)]
    unsafe {
        libc::_exit(code)
    }
    #[cfg(not(unix))]
    std::process::exit(code)
}

impl App {
    fn vm(&self) -> Option<Qemu> {
        match &self.source {
            Some(Source::Qemu { vm, .. }) if !self.closing => Some(*vm),
            _ => None,
        }
    }

    /// Orderly exit: QEMU must finish `qemu_cleanup` before the process
    /// exits, or QEMU's own atexit handlers (audio_cleanup, exit notifiers)
    /// run on this thread concurrently with the main loop — seen on macOS as
    /// `assertion failed: mutex->initialized` in qemu_mutex_lock_impl.
    fn join_qemu(&mut self) -> i32 {
        self.closing = true;
        if let Some(Source::Qemu { display, .. }) = &self.source {
            display.release(); // no more calls from this thread: cleanup may free
        }
        let Some(handle) = self.qemu_thread.take() else {
            return 0;
        };
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(15);
        while !handle.is_finished() {
            if std::time::Instant::now() > deadline {
                eprintln!("[player] QEMU did not shut down in 15 s; exiting without cleanup");
                hard_exit(1);
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        handle.join().unwrap_or(1)
    }

    /// Release every key the guest still sees as held (focus loss).
    fn lift_all_keys(&mut self) {
        let keys = std::mem::take(&mut self.keys_down);
        if keys.is_empty() {
            return;
        }
        if let Some(vm) = self.vm() {
            for qcode in keys {
                vm.key(qcode, false);
            }
            vm.input_flush();
        }
    }

    fn set_grab(&mut self, on: bool) {
        let Some(gpu) = &self.gpu else { return };
        if on {
            if gpu.window.set_cursor_grab(CursorGrabMode::Locked).is_err() {
                let _ = gpu.window.set_cursor_grab(CursorGrabMode::Confined);
            }
            gpu.window.set_cursor_visible(false);
            gpu.window
                .set_title("win98-xp-virt player — mouse grabbed (Ctrl+Alt+G releases)");
        } else {
            let _ = gpu.window.set_cursor_grab(CursorGrabMode::None);
            gpu.window.set_cursor_visible(true);
            gpu.window.set_title("win98-xp-virt player");
        }
        self.grabbed = on;
    }

    /// Window pixel → guest framebuffer coordinates (None outside the image).
    /// Pick up a new guest cursor shape (a define or a clear) and turn it
    /// into a host cursor; needs the event loop, so it runs on wakes.
    fn update_guest_cursor(&mut self, event_loop: &ActiveEventLoop) {
        let Some(Source::Qemu { display, .. }) = &self.source else { return };
        let Some((seq, shape)) = display.cursor_if_newer(self.guest_cursor_seq) else { return };
        self.guest_cursor_seq = seq;
        self.guest_cursor = shape.and_then(|c| {
            let mut rgba = Vec::with_capacity(c.argb.len() * 4);
            for px in &c.argb {
                rgba.extend_from_slice(&[(px >> 16) as u8, (px >> 8) as u8, *px as u8, (px >> 24) as u8]);
            }
            match CustomCursor::from_rgba(rgba, c.width as u16, c.height as u16, c.hot_x as u16, c.hot_y as u16) {
                Ok(src) => Some(event_loop.create_custom_cursor(src)),
                Err(e) => {
                    eprintln!("[cursor] guest shape {}x{} refused: {e}", c.width, c.height);
                    None
                }
            }
        });
        self.apply_cursor();
    }

    /// The host cursor over the window: the guest's shape while over the
    /// image (and visible per the guest), hidden over the image when the
    /// guest has no hardware cursor, the default elsewhere.
    /// The host pointer sits exactly where the guest's cursor is only with an
    /// absolute device (the USB tablet) and no grab: then the guest's shape
    /// can be the host cursor. Otherwise (a relative mouse, PS/2, whether
    /// grabbed or not) the sprite is composited into the frame.
    fn host_cursor_possible(&self) -> bool {
        !self.grabbed && self.vm().map(|v| v.mouse_is_absolute()).unwrap_or(false)
    }

    fn apply_cursor(&mut self) {
        let Some(gpu) = &self.gpu else { return };
        if self.grabbed {
            return;
        }
        let visible = match &self.source {
            Some(Source::Qemu { display, .. }) => display.cursor_visible(),
            _ => None,
        };
        let want = if !self.pointer_inside {
            HostCursor::Default
        } else if let (true, Some(_), Some(true)) = (self.host_cursor_possible(), &self.guest_cursor, visible) {
            HostCursor::Guest(self.guest_cursor_seq)
        } else {
            HostCursor::Hidden
        };
        if want == self.cursor_applied {
            return;
        }
        if std::env::var("PLAYER_CURSOR_LOG").is_ok() {
            eprintln!(
                "[cursor] host: {}",
                match &want {
                    HostCursor::Default => "default".to_string(),
                    HostCursor::Hidden => format!("hidden (shape {}, guest visible {:?})", self.guest_cursor.is_some(), visible),
                    HostCursor::Guest(s) => format!("the guest's shape #{s}"),
                }
            );
        }
        match &want {
            HostCursor::Default => {
                gpu.window.set_cursor(Cursor::default());
                gpu.window.set_cursor_visible(true);
            }
            HostCursor::Guest(_) => {
                gpu.window.set_cursor(Cursor::Custom(self.guest_cursor.clone().unwrap()));
                gpu.window.set_cursor_visible(true);
            }
            HostCursor::Hidden => gpu.window.set_cursor_visible(false),
        }
        self.cursor_applied = want;
    }

    fn to_guest(&self, px: f64, py: f64) -> Option<(i32, i32, i32, i32)> {
        let gpu = self.gpu.as_ref()?;
        let (_, _, tw, th) = gpu.current()?;
        let (tw, th) = (*tw, *th);
        let (x, y, w, h) = gpu.viewport();
        let gx = ((px as f32 - x) / w * tw as f32) as i32;
        let gy = ((py as f32 - y) / h * th as f32) as i32;
        if gx < 0 || gy < 0 || gx >= tw as i32 || gy >= th as i32 {
            return None;
        }
        Some((gx, gy, tw as i32, th as i32))
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.gpu.is_some() {
            return;
        }
        let attrs = Window::default_attributes()
            .with_title("win98-xp-virt player")
            .with_inner_size(LogicalSize::new(1280.0, 960.0));
        let window = Arc::new(event_loop.create_window(attrs).expect("create window"));

        let mut gpu = Gpu::new(window);

        if let Some(p) = &self.shader {
            gpu.load_shader(p);
        }

        if let Some(target) = self.calib.clone() {
            let mut files: Vec<std::path::PathBuf> = if target.is_dir() {
                std::fs::read_dir(&target)
                    .map(|rd| {
                        rd.filter_map(|e| e.ok().map(|e| e.path()))
                            .filter(|p| {
                                p.extension().and_then(|e| e.to_str()) == Some("bmp")
                            })
                            .collect()
                    })
                    .unwrap_or_default()
            } else {
                vec![target.clone()]
            };
            files.sort();
            if files.is_empty() {
                eprintln!("calib: no .bmp in {}", target.display());
                hard_exit(1);
            }
            if self.shader.is_none() {
                eprintln!("calib: --shader is the whole point; nothing to shade with");
                hard_exit(1);
            }
            gpu.forced_surface = Some(SWEEP_SURFACE);
            println!("shading {} calibration pattern(s)", files.len());
            self.gpu = Some(gpu);
            self.source = Some(Source::Calib(Calib {
                files,
                i: 0,
                last_surface: (0, 0),
            }));
            return;
        }
        if let Some(dir) = self.sweep.clone() {
            if let Err(e) = std::fs::create_dir_all(&dir) {
                eprintln!("mode sweep: {}: {e}", dir.display());
                hard_exit(1);
            }
            // big enough that every mode in the table gets at least two
            // output pixels per scanline, so the count is measurable for all
            // of them rather than only the low-resolution ones
            gpu.forced_surface = Some(SWEEP_SURFACE);
            self.gpu = Some(gpu);
            println!(
                "mode sweep into {} at {}x{}",
                dir.display(),
                SWEEP_SURFACE.0,
                SWEEP_SURFACE.1
            );
            self.source = Some(Source::Sweep(Sweep::new(dir, self.shader.is_some())));
            return;
        }
        self.gpu = Some(gpu);
        self.source = Some(if self.qemu_args.is_empty() {
            Source::Pattern(Pattern::new())
        } else {
            // host audio first: QEMU's audiodev must match the device rate
            let ring = audio::Ring::new();
            let audio_out = audio::start(ring.clone());
            if audio_out.is_none() {
                eprintln!("[audio] no output device; guest audio disabled");
            }
            self.audio = audio_out;
            let audio_cfg = self.audio.as_ref().map(|o| (ring, o.sample_rate));
            // Render on publish, not on a free-running redraw: a frame that
            // waits for the next loop iteration adds up to one host frame of
            // latency before it even reaches the swapchain (doc 03).
            let waker = self.proxy.clone().map(|p| {
                Arc::new(move || {
                    let _ = p.send_event(());
                }) as Arc<dyn Fn() + Send + Sync>
            });
            let zero_copy = self.gpu.as_ref().map(|g| g.zero_copy).unwrap_or(false);
            let (vm, display, join, qmp) =
                qemu_vm::start(self.qemu_args.clone(), audio_cfg, waker, zero_copy);
            self.qemu_thread = Some(join);
            Source::Qemu {
                vm,
                display,
                last_seq: 0,
                qmp,
                qmp_exec_done: false,
            }
        });
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => {
                if let Some(vm) = self.vm() {
                    vm.vm_shutdown();
                }
                self.closing = true;
                event_loop.exit();
            }
            WindowEvent::Resized(size) => {
                if let Some(gpu) = self.gpu.as_mut() {
                    gpu.resize(size.width, size.height);
                }
            }
            WindowEvent::ModifiersChanged(m) => self.modifiers = m.state(),
            WindowEvent::KeyboardInput { event, .. } => {
                let PhysicalKey::Code(code) = event.physical_key else {
                    return;
                };
                let down = event.state == ElementState::Pressed;
                // Ctrl+Alt+G: release the mouse grab (host-side hotkey)
                if down
                    && code == KeyCode::KeyG
                    && self.modifiers.control_key()
                    && self.modifiers.alt_key()
                {
                    self.set_grab(false);
                    return;
                }
                if let (Some(vm), Some(sc)) = (self.vm(), keymap::atset1(code)) {
                    let qcode = qemu_embed::atset1_to_qcode(sc);
                    if qcode != 0 {
                        vm.key(qcode, down);
                        vm.input_flush();
                        if down {
                            if !self.keys_down.contains(&qcode) {
                                self.keys_down.push(qcode);
                            }
                        } else {
                            self.keys_down.retain(|&k| k != qcode);
                        }
                    }
                }
            }
            WindowEvent::CursorMoved { position, .. } => {
                if let Some(vm) = self.vm() {
                    if vm.mouse_is_absolute() {
                        if self.grabbed {
                            // guest switched to a tablet: a relative grab is wrong now
                            self.set_grab(false);
                        }
                        let inside = self.to_guest(position.x, position.y);
                        // over the image the host cursor is the guest's hardware
                        // cursor, or hidden while the guest paints its own
                        if self.pointer_inside != inside.is_some() {
                            self.pointer_inside = inside.is_some();
                            self.apply_cursor();
                        }
                        if let Some((x, y, w, h)) = inside {
                            vm.mouse_abs(x, y, w, h);
                            vm.input_flush();
                        }
                    }
                }
            }
            WindowEvent::CursorLeft { .. } => {
                self.pointer_inside = false;
                self.apply_cursor();
            }
            WindowEvent::MouseInput { state, button, .. } => {
                let down = state == ElementState::Pressed;
                if let Some(vm) = self.vm() {
                    if down && !self.grabbed && !vm.mouse_is_absolute() {
                        self.set_grab(true);
                    }
                    let b = match button {
                        MouseButton::Left => 0,
                        MouseButton::Middle => 1,
                        MouseButton::Right => 2,
                        MouseButton::Back => 5,
                        MouseButton::Forward => 6,
                        MouseButton::Other(_) => return,
                    };
                    vm.mouse_btn(b, down);
                    vm.input_flush();
                }
            }
            WindowEvent::MouseWheel { delta, .. } => {
                if let Some(vm) = self.vm() {
                    let y = match delta {
                        MouseScrollDelta::LineDelta(_, y) => y,
                        MouseScrollDelta::PixelDelta(p) => p.y as f32 / 40.0,
                    };
                    let b = if y > 0.0 {
                        3
                    } else if y < 0.0 {
                        4
                    } else {
                        return;
                    };
                    vm.mouse_btn(b, true);
                    vm.mouse_btn(b, false);
                    vm.input_flush();
                }
            }
            WindowEvent::Focused(false) => {
                self.set_grab(false);
                self.lift_all_keys();
            }
            WindowEvent::RedrawRequested => {
                let sprite = !self.host_cursor_possible();
                let Some(gpu) = self.gpu.as_mut() else { return };
                // The sweep never presents: it reads the chain's output
                // texture back instead. It must not acquire either — with
                // FIFO the second acquire blocks until the first image has
                // been scanned out, which an occluded window (a test run
                // behind a terminal, the usual case) never does.
                let sweeping = matches!(
                    self.source,
                    Some(Source::Sweep(_)) | Some(Source::Calib(_))
                );
                let frame = if sweeping {
                    None
                } else {
                    match gpu.acquire() {
                        Some(f) => Some(f),
                        None => return,
                    }
                };
                let mut published = None;
                match self.source.as_mut() {
                    Some(Source::Pattern(p)) => {
                        p.render();
                        gpu.upload(&p.fb, pattern::WIDTH as u32, pattern::HEIGHT as u32);
                    }
                    Some(Source::Sweep(s)) => sweep_upload(gpu, s),
                    Some(Source::Calib(c)) => match read_bmp(&c.files[c.i]) {
                        Ok((w, h, px)) => gpu.upload(&px, w, h),
                        Err(e) => {
                            eprintln!("calib: {e}");
                            hard_exit(1);
                        }
                    },
                    Some(Source::Qemu {
                        display, last_seq, ..
                    }) => {
                        published = present_guest_frame(gpu, display, last_seq, sprite);
                    }
                    None => {}
                }
                gpu.render(frame);
                if let Some(t) = published {
                    // publish→present (measured after the present call) — doc 03 latency gate
                    self.latency.push(t.elapsed().as_secs_f32() * 1000.0);
                    if self.latency.len() >= 240 {
                        if std::env::var("PLAYER_LATENCY").is_ok() {
                            let mut v = self.latency.clone();
                            v.sort_by(|a, b| a.partial_cmp(b).unwrap());
                            eprintln!(
                                "[latency] publish→present p50 {:.1} ms  p95 {:.1} ms  max {:.1} ms (n={})",
                                v[v.len() / 2],
                                v[v.len() * 95 / 100],
                                v[v.len() - 1],
                                v.len()
                            );
                        }
                        self.latency.clear();
                    }
                }
                if let Some(Source::Calib(c)) = self.source.as_mut() {
                    if c.last_surface != gpu.surface_size() {
                        c.last_surface = gpu.surface_size();
                    } else {
                        let src = c.files[c.i].clone();
                        match gpu.chain.as_ref().and_then(|ch| ch.output()) {
                            Some(tex) => {
                                let (ow, oh, rgb) =
                                    shader::read_texture(&gpu.device, &gpu.queue, tex);
                                let out = src.with_extension("shaded.png");
                                shader::write_png(&out.to_string_lossy(), ow, oh, &rgb);
                                println!("  {} → {} ({ow}x{oh})",
                                         src.file_name().unwrap_or_default().to_string_lossy(),
                                         out.display());
                            }
                            None => {
                                eprintln!("calib: the preset did not load");
                                hard_exit(1);
                            }
                        }
                        c.i += 1;
                        if c.i >= c.files.len() {
                            hard_exit(0);
                        }
                    }
                }
                if let Some(Source::Sweep(s)) = self.source.as_mut() {
                    if sweep_step(gpu, s) {
                        if s.fails.is_empty() {
                            println!("mode sweep: {} modes OK", s.sizes.len());
                            hard_exit(0);
                        }
                        println!("mode sweep: {} of {} modes failed", s.fails.len(), s.sizes.len());
                        hard_exit(1);
                    }
                }
                if matches!(
                    self.source,
                    Some(Source::Pattern(_)) | Some(Source::Sweep(_)) | Some(Source::Calib(_))
                ) {
                    gpu.window.request_redraw();
                }
            }
            _ => {}
        }
    }

    fn user_event(&mut self, event_loop: &ActiveEventLoop, _ev: ()) {
        // the guest's cursor shape or visibility may have changed (a wake
        // comes for both; the shape needs the event loop to become a cursor)
        self.update_guest_cursor(event_loop);
        if self.pointer_inside {
            self.apply_cursor();
        }
        // dma-buf ring slots are imported here, not on redraw: an occluded
        // window gets no usable swapchain image but must still keep up
        if let (Some(gpu), Some(Source::Qemu { display, .. })) = (self.gpu.as_mut(), &self.source) {
            for d in display.take_dmabufs() {
                gpu.import_slot(&d);
            }
        }
        // QEMU's main loop returned (guest power-off, `quit`): stop touching
        // the handle and leave; main() then releases it for qemu_cleanup.
        if let Some(Source::Qemu { display, .. }) = &self.source {
            if display.stopped() && !self.closing {
                self.closing = true;
                event_loop.exit();
                return;
            }
        }
        if let Some(Source::Qemu {
            qmp: Some(qmp),
            qmp_exec_done,
            last_seq,
            ..
        }) = &mut self.source
        {
            // QMP events: all of them under PLAYER_QMP=1, the notable ones always
            let verbose = std::env::var("PLAYER_QMP").is_ok();
            for ev in qmp.take_events() {
                let name = ev["event"].as_str().unwrap_or("?");
                if verbose || qmp::is_notable(name) {
                    eprintln!("[qmp] event {name} {}", ev.get("data").unwrap_or(&serde_json::Value::Null));
                }
            }
            // PLAYER_QMP_EXEC: one request object or an array of them, run once
            // the guest has drawn — a shell-level way to try commands
            // (eject, blockdev-change-medium, snapshot-save, ...).
            if !*qmp_exec_done && *last_seq > 0 {
                *qmp_exec_done = true;
                if let Ok(spec) = std::env::var("PLAYER_QMP_EXEC") {
                    match serde_json::from_str::<serde_json::Value>(&spec) {
                        Ok(serde_json::Value::Array(reqs)) => {
                            for r in &reqs {
                                eprintln!("[qmp] {r} -> {:?}", qmp.execute_raw(r));
                            }
                        }
                        Ok(r) => eprintln!("[qmp] {r} -> {:?}", qmp.execute_raw(&r)),
                        Err(e) => eprintln!("[qmp] PLAYER_QMP_EXEC is not JSON: {e}"),
                    }
                }
            }
        }
        // QEMU published a frame (multiple wakes coalesce into one redraw)
        if let Some(gpu) = &self.gpu {
            gpu.window.request_redraw();
        }
    }

    fn about_to_wait(&mut self, _el: &ActiveEventLoop) {
        // Headless verification (PLAYER_DUMP_OUT): an occluded window may never
        // get RedrawRequested, but the shader chain renders into our own
        // texture, so drive the frame from here in that mode.
        if std::env::var("PLAYER_DUMP_OUT").is_ok() {
            if let Some(Source::Qemu {
                display, last_seq, ..
            }) = self.source.as_mut()
            {
                if let Some(gpu) = self.gpu.as_mut() {
                    if present_guest_frame(gpu, display, last_seq, true).is_some() {
                        gpu.render(None);
                    }
                }
            }
        }
    }

    fn device_event(&mut self, _el: &ActiveEventLoop, _id: DeviceId, event: DeviceEvent) {
        if let DeviceEvent::MouseMotion { delta: (dx, dy) } = event {
            if !self.grabbed {
                return;
            }
            if let Some(vm) = self.vm() {
                if !vm.mouse_is_absolute() {
                    vm.mouse_rel(dx as i32, dy as i32);
                    vm.input_flush();
                }
            }
        }
    }
}

/// Debug: PLAYER_DUMP=<file.png> writes guest frame #PLAYER_DUMP_SEQ (default
/// 60) from the staging buffer, then exits. Lets CI/agents verify a boot.
pub fn maybe_dump(pixels: &[u32], w: usize, h: usize, seq: u64) {
    let Ok(path) = std::env::var("PLAYER_DUMP") else {
        return;
    };
    let want: u64 = std::env::var("PLAYER_DUMP_SEQ")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(60);
    if seq < want {
        return;
    }
    let mut rgb = Vec::with_capacity(w * h * 3);
    for p in pixels {
        rgb.extend_from_slice(&[(p >> 16) as u8, (p >> 8) as u8, *p as u8]);
    }
    let file = std::fs::File::create(&path).expect("dump file");
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w as u32, h as u32);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().unwrap().write_image_data(&rgb).unwrap();
    eprintln!("dumped {w}x{h} frame #{seq} to {path}");
    hard_exit(0);
}

fn main() {
    // player [--shader <preset.slangp>] [--mode-sweep <dir>] [--calib <bmp|dir>]
    //        [--] <qemu args...>
    //   no args: the M0 test pattern; --mode-sweep: doc 03's mode sweep;
    //   --calib: shade doc 09's calibration patterns. All three: no guest.
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut shader: Option<std::path::PathBuf> =
        std::env::var("PLAYER_SHADER").ok().map(Into::into);
    if args.first().map(String::as_str) == Some("--shader") && args.len() >= 2 {
        shader = Some(args[1].clone().into());
        args.drain(0..2);
    }
    let mut sweep = None;
    if args.first().map(String::as_str) == Some("--mode-sweep") && args.len() >= 2 {
        sweep = Some(std::path::PathBuf::from(args[1].clone()));
        args.drain(0..2);
    }
    let mut calib = None;
    if args.first().map(String::as_str) == Some("--calib") && args.len() >= 2 {
        calib = Some(std::path::PathBuf::from(args[1].clone()));
        args.drain(0..2);
    }
    if args.first().map(String::as_str) == Some("--") {
        args.remove(0);
    }
    let event_loop = EventLoop::<()>::with_user_event().build().expect("event loop");
    event_loop.set_control_flow(ControlFlow::Wait);
    let mut app = App {
        qemu_args: args,
        shader,
        sweep,
        calib,
        proxy: Some(event_loop.create_proxy()),
        ..Default::default()
    };
    event_loop.run_app(&mut app).expect("run");
    let status = app.join_qemu();
    // Return, don't exit(): QEMU's atexit handlers run here, after its
    // thread has already completed qemu_cleanup — the same order as
    // qemu-system's own main().
    if status != 0 {
        std::process::exit(status);
    }
}
