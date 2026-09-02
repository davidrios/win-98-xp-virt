//! win98-xp-virt player (doc 02): one running machine per process.
//!
//! M1 state: `player -- <qemu-system args>` boots QEMU in-process and
//! presents the guest framebuffer through wgpu (integer-scaled 4:3);
//! keyboard and mouse are injected. No args → the M0 test pattern.

mod keymap;
mod pattern;
mod qemu_vm;

use pattern::Pattern;
use qemu_embed::Qemu;
use std::sync::Arc;
use winit::application::ApplicationHandler;
use winit::dpi::LogicalSize;
use winit::event::{
    DeviceEvent, DeviceId, ElementState, MouseButton, MouseScrollDelta, WindowEvent,
};
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
use winit::keyboard::{KeyCode, ModifiersState, PhysicalKey};
use winit::window::{CursorGrabMode, Window, WindowId};

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
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("player"),
            ..Default::default()
        }))
        .expect("request device");

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
            format: wgpu::TextureFormat::Bgra8Unorm,
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

    /// Largest integer-scaled 4:3 rect that fits the surface, centered
    /// (doc 03 geometry stage; pixel-aspect table comes in M2).
    fn viewport(&self) -> (f32, f32, f32, f32) {
        let Some((_, _, tw, th)) = self.fb_tex.as_ref() else {
            return (0.0, 0.0, 1.0, 1.0);
        };
        let (tw, th) = (*tw, *th);
        let (sw, sh) = (self.config.width as f32, self.config.height as f32);
        let (gw, gh) = (tw as f32, th as f32);
        let scale = (sw / gw).min(sh / gh).floor().max(1.0);
        let (vw, vh) = (gw * scale, gh * scale);
        ((sw - vw) / 2.0, (sh - vh) / 2.0, vw, vh)
    }

    fn render(&mut self) {
        let Some((_, bg, _, _)) = &self.fb_tex else {
            return;
        };
        use wgpu::CurrentSurfaceTexture as Cst;
        let frame = match self.surface.get_current_texture() {
            Cst::Success(f) | Cst::Suboptimal(f) => f,
            Cst::Timeout | Cst::Occluded => return,
            _ => {
                self.resize(self.config.width, self.config.height);
                return;
            }
        };
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
    Qemu {
        vm: Qemu,
        display: qemu_vm::Display,
        last_seq: u64,
    },
}

#[derive(Default)]
struct App {
    qemu_args: Vec<String>,
    gpu: Option<Gpu>,
    source: Option<Source>,
    modifiers: ModifiersState,
    grabbed: bool,
}

impl App {
    fn vm(&self) -> Option<Qemu> {
        match &self.source {
            Some(Source::Qemu { vm, .. }) => Some(*vm),
            _ => None,
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
    fn to_guest(&self, px: f64, py: f64) -> Option<(i32, i32, i32, i32)> {
        let gpu = self.gpu.as_ref()?;
        let (_, _, tw, th) = gpu.fb_tex.as_ref()?;
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
        self.gpu = Some(Gpu::new(window));
        self.source = Some(if self.qemu_args.is_empty() {
            Source::Pattern(Pattern::new())
        } else {
            let (vm, display, _join) = qemu_vm::start(self.qemu_args.clone());
            Source::Qemu {
                vm,
                display,
                last_seq: 0,
            }
        });
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => {
                if let Some(vm) = self.vm() {
                    vm.vm_shutdown();
                }
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
                    }
                }
            }
            WindowEvent::CursorMoved { position, .. } => {
                if let Some(vm) = self.vm() {
                    if vm.mouse_is_absolute() {
                        if let Some((x, y, w, h)) = self.to_guest(position.x, position.y) {
                            vm.mouse_abs(x, y, w, h);
                            vm.input_flush();
                        }
                    }
                }
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
            WindowEvent::Focused(false) => self.set_grab(false),
            WindowEvent::RedrawRequested => {
                let Some(gpu) = self.gpu.as_mut() else { return };
                match self.source.as_mut() {
                    Some(Source::Pattern(p)) => {
                        p.render();
                        gpu.upload(&p.fb, pattern::WIDTH as u32, pattern::HEIGHT as u32);
                    }
                    Some(Source::Qemu {
                        display, last_seq, ..
                    }) => {
                        if let Some(f) = display.take_if_newer(*last_seq) {
                            *last_seq = f.seq;
                            gpu.upload(&f.pixels, f.width as u32, f.height as u32);
                        }
                    }
                    None => {}
                }
                gpu.render();
                gpu.window.request_redraw();
            }
            _ => {}
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
    std::process::exit(0);
}

fn main() {
    // player [--] <qemu-system-i386 args...>   (no args: test pattern)
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--") {
        args.remove(0);
    }
    let event_loop = EventLoop::new().expect("event loop");
    event_loop.set_control_flow(ControlFlow::Poll);
    let mut app = App {
        qemu_args: args,
        ..Default::default()
    };
    event_loop.run_app(&mut app).expect("run");
}
