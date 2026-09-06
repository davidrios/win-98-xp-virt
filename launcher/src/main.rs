//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf, snapshots, shader profiles — the **egui/eframe** front end.
//!
//! Everything this program *decides* is `launcher-core`, which the Qt
//! front end (`launcher-qt/`) drives too: the bundle format, the
//! library, the disc shelf, the snapshot state machine, the wizard's
//! form, the profile editor, the shader preview's render path, and every
//! debug verb that needs no toolkit (`launcher_core::cli`). What is in
//! this crate is egui: the grid, the windows, the widgets, an OS file
//! dialog egui doesn't have (`filepicker.rs`), and the diagnostic verbs
//! below that render real frames without a window.

mod discshelf;
mod filepicker;
mod shader_manager;
mod shader_preview;
mod snapshots_ui;
mod wizard;

// The shared half, reachable as `crate::…` from the modules above the
// way it was when these were files in this crate.
use launcher_core::machines::Machines;
use launcher_core::{cli, disc_library, library, paths, shader_library};
use std::path::{Path, PathBuf};

struct LauncherApp {
    machines: Machines,
    wizard: launcher_core::wizard::Form,
    disc_shelf: discshelf::DiscShelfWindow,
    snapshots: snapshots_ui::SnapshotWindow,
    shader_manager: shader_manager::ShaderManager,
    /// `None` on a non-wgpu eframe backend (not expected in practice —
    /// `wgpu` is a default feature, see `docs/tracks/m6-launcher.md` —
    /// but the shader profile editor degrades to "no live preview"
    /// rather than unwrapping this).
    wgpu_render_state: Option<eframe::egui_wgpu::RenderState>,
}

/// One row of the grid, pulled out of the model before it is drawn: the
/// row loop wants to call back into `Machines` (Play), and it cannot
/// hold a borrow of the entries while doing that.
struct Row {
    name: String,
    family: &'static str,
    shader: String,
    location: String,
    running: bool,
}

/// What a click on a row asked for, applied after the grid has been laid
/// out for the same reason.
enum RowAction {
    Play(usize),
    Edit(usize),
    Discs(usize),
    Snapshots(usize),
}

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        // No push notification from a child exiting: poll for it instead.
        ctx.request_repaint_after(std::time::Duration::from_millis(500));
        self.machines.reap();

        let rows: Vec<Row> = self
            .machines
            .entries()
            .iter()
            .enumerate()
            .map(|(i, entry)| Row {
                name: entry.machine.name.clone(),
                family: entry.machine.family.label(),
                shader: self.machines.shader_label(entry),
                location: entry.dir.display().to_string(),
                running: self.machines.is_running(i),
            })
            .collect();
        let mut action = None;

        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading(paths::NAME);
            ui.add_space(8.0);
            if rows.is_empty() {
                ui.label("No machines yet.");
                ui.label(format!("Library: {}", self.machines.library_dir.display()));
            } else {
                egui::Grid::new("library").striped(true).show(ui, |ui| {
                    ui.strong("Name");
                    ui.strong("Family");
                    ui.strong("Shader");
                    ui.strong("Location");
                    ui.strong("");
                    ui.end_row();
                    for (i, row) in rows.iter().enumerate() {
                        ui.label(&row.name);
                        ui.label(row.family);
                        ui.label(&row.shader);
                        ui.label(&row.location);
                        ui.horizontal(|ui| {
                            if row.running {
                                ui.label("Running");
                            } else if ui.button("Play").clicked() {
                                action = Some(RowAction::Play(i));
                            }
                            if ui.button("Edit…").clicked() {
                                action = Some(RowAction::Edit(i));
                            }
                            if ui.button("Discs…").clicked() {
                                action = Some(RowAction::Discs(i));
                            }
                            if ui.button("Snapshots…").clicked() {
                                action = Some(RowAction::Snapshots(i));
                            }
                        });
                        ui.end_row();
                    }
                });
            }
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                if ui.button("New machine…").clicked() {
                    self.wizard.open_fresh();
                }
                if ui.button("Disc shelf…").clicked() {
                    self.disc_shelf.shelf.open_library(&self.machines.disc_library_path);
                }
                if ui.button("Shader profiles…").clicked() {
                    self.shader_manager.open_list();
                }
            });
        });

        self.apply(action);
        self.windows(&ctx);
    }
}

impl LauncherApp {
    fn apply(&mut self, action: Option<RowAction>) {
        let Some(action) = action else { return };
        match action {
            RowAction::Play(row) => {
                if let Err(e) = self.machines.play(row) {
                    eprintln!("[launcher] {e}");
                }
            }
            RowAction::Edit(row) => {
                if let Some(path) = self.machines.bundle_path(row) {
                    self.wizard.open_edit_path(path);
                }
            }
            RowAction::Discs(row) => {
                if let Some(path) = self.machines.bundle_path(row) {
                    self.disc_shelf.shelf.open_for_path(path, &self.machines.disc_library_path);
                }
            }
            RowAction::Snapshots(row) => {
                if let Some(path) = self.machines.bundle_path(row) {
                    let running = self.machines.is_running(row);
                    self.snapshots.model.open_for_path(&path, running);
                }
            }
        }
    }

    fn windows(&mut self, ctx: &egui::Context) {
        if wizard::show(&mut self.wizard, ctx, &self.machines.library_dir.clone(), self.machines.profiles()).is_some() {
            self.machines.refresh();
        }
        // Each per-machine window is one instance the app owns, so
        // whether *that* machine is running is looked up by the bundle
        // it has open rather than carried around with it.
        let shelf_running =
            self.disc_shelf.shelf.bundle_dir().map(|dir| self.machines.is_running_dir(dir)).unwrap_or(false);
        if self.disc_shelf.show(ctx, shelf_running).is_some() {
            self.machines.refresh();
        }
        if self.disc_shelf.shelf.take_saved() {
            // A disc added or renamed should show up in the guest's own
            // CDSHELF listing without restarting the machine, so every
            // running drive gets the new shelf file.
            self.machines.republish_shelf();
        }
        let snapshots_running =
            self.snapshots.model.bundle_dir().map(|dir| self.machines.is_running_dir(dir)).unwrap_or(false);
        self.snapshots.show(ctx, snapshots_running);
        if self
            .shader_manager
            .show(ctx, &self.machines.profiles_dir.clone(), self.wgpu_render_state.as_ref())
            .is_some()
        {
            self.machines.refresh_profiles();
        }
    }
}

/// A windowless wgpu device/queue, the same way eframe opens one at
/// startup — for the diagnostic verbs below, which run real egui frames
/// with no window to put them in.
fn headless_render_state() -> eframe::egui_wgpu::RenderState {
    let instance =
        eframe::wgpu::Instance::new(eframe::wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    pollster::block_on(eframe::egui_wgpu::RenderState::create(
        &eframe::egui_wgpu::WgpuConfiguration::default(),
        &instance,
        None,
        eframe::egui_wgpu::RendererOptions::default(),
    ))
    .expect("create a headless wgpu render state")
}

/// Hands egui's renderer the textures a just-run frame created or
/// dropped (the font atlas on the first frame, mostly). Every frame's
/// deltas must be applied even when only the last one is painted, or
/// that last frame paints text with no atlas.
fn apply_texture_deltas(render_state: &eframe::egui_wgpu::RenderState, delta: &mut egui::TexturesDelta) {
    let mut renderer = render_state.renderer.write();
    for (id, deltas) in &delta.set {
        for d in deltas {
            renderer.update_texture(&render_state.device, &render_state.queue, *id, d);
        }
    }
    for id in &delta.free {
        renderer.free_texture(id);
    }
    delta.clear(); // also avoids the debug-only "unapplied deltas" assert on drop
}

/// Paints one already-run egui frame into an off-screen texture and
/// dumps it as a PNG — the paint step eframe would do, minus the window.
fn dump_egui_frame(
    render_state: &eframe::egui_wgpu::RenderState,
    ctx: &egui::Context,
    mut full_output: egui::FullOutput,
    size: [u32; 2],
    out: &str,
) {
    let clipped = ctx.tessellate(full_output.shapes, full_output.pixels_per_point);
    let screen_descriptor =
        eframe::egui_wgpu::ScreenDescriptor { size_in_pixels: size, pixels_per_point: full_output.pixels_per_point };
    let target = render_state.device.create_texture(&eframe::wgpu::TextureDescriptor {
        label: Some("diag frame"),
        size: eframe::wgpu::Extent3d { width: size[0], height: size[1], depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: eframe::wgpu::TextureDimension::D2,
        format: eframe::wgpu::TextureFormat::Rgba8Unorm,
        usage: eframe::wgpu::TextureUsages::RENDER_ATTACHMENT | eframe::wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let target_view = target.create_view(&eframe::wgpu::TextureViewDescriptor::default());
    let mut encoder =
        render_state.device.create_command_encoder(&eframe::wgpu::CommandEncoderDescriptor { label: Some("diag") });
    apply_texture_deltas(render_state, &mut full_output.textures_delta);
    let user_cmd_bufs = {
        let mut renderer = render_state.renderer.write();
        renderer.update_buffers(&render_state.device, &render_state.queue, &mut encoder, &clipped, &screen_descriptor)
    };
    {
        let renderer = render_state.renderer.read();
        let render_pass = encoder.begin_render_pass(&eframe::wgpu::RenderPassDescriptor {
            label: Some("diag"),
            color_attachments: &[Some(eframe::wgpu::RenderPassColorAttachment {
                view: &target_view,
                resolve_target: None,
                ops: eframe::wgpu::Operations {
                    load: eframe::wgpu::LoadOp::Clear(eframe::wgpu::Color { r: 0.2, g: 0.2, b: 0.2, a: 1.0 }),
                    store: eframe::wgpu::StoreOp::Store,
                },
                depth_slice: None,
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        renderer.render(&mut render_pass.forget_lifetime(), &clipped, &screen_descriptor);
    }
    render_state.queue.submit(user_cmd_bufs.into_iter().chain(std::iter::once(encoder.finish())));
    shader_chain::dump_texture(&render_state.device, &render_state.queue, &target, out);
}

/// One step of a diagnostic verb's synthetic input script.
enum DiagAction {
    /// A primary-button click at a screen position.
    Click(egui::Pos2),
    /// Text typed into whatever a preceding click focused.
    Type(String),
    /// Wall-clock wait before the next step, for a window whose state is
    /// changed by something running off the UI thread — the shader
    /// download is the one that needs it.
    Wait(std::time::Duration),
}

/// Drives one of the app's windows through egui headlessly with
/// synthetic input and dumps the composited frame — the way this front
/// end takes a screenshot without a window at all. (The Qt build gets
/// the same thing from `QT_QPA_PLATFORM=offscreen` plus
/// `Item.grabToImage()`: Qt separates "render" from "have a window" and
/// egui does not, which is why this is ~150 lines here and four of QML
/// there.) Each action gets its own frames, because egui needs a frame
/// to notice a widget was pressed and another to react.
fn diag_window_frames(
    render_state: &eframe::egui_wgpu::RenderState,
    screen: egui::Vec2,
    script: &[DiagAction],
    out: &str,
    mut window: impl FnMut(&egui::Context),
) {
    let ctx = egui::Context::default();
    let screen_rect = egui::Rect::from_min_size(egui::Pos2::ZERO, screen);
    let mut run = |events: Vec<egui::Event>, paint: bool| {
        let input = egui::RawInput { screen_rect: Some(screen_rect), events, ..Default::default() };
        let mut full_output = ctx.run_ui(input, |ui| window(ui.ctx()));
        if paint {
            dump_egui_frame(render_state, &ctx, full_output, [screen.x as u32, screen.y as u32], out);
        } else {
            apply_texture_deltas(render_state, &mut full_output.textures_delta);
        }
    };
    let button = |pos, pressed| egui::Event::PointerButton {
        pos,
        button: egui::PointerButton::Primary,
        pressed,
        modifiers: egui::Modifiers::default(),
    };
    run(Vec::new(), false);
    run(Vec::new(), false); // settle: egui needs a frame to lay a new window out
    for action in script {
        match action {
            DiagAction::Click(pos) => {
                println!("click at {:.0},{:.0}", pos.x, pos.y);
                run(vec![egui::Event::PointerMoved(*pos)], false);
                run(vec![button(*pos, true)], false);
                run(vec![button(*pos, false)], false);
                run(vec![egui::Event::PointerGone], false);
            }
            DiagAction::Type(text) => {
                println!("type {text:?}");
                run(vec![egui::Event::Text(text.clone())], false);
            }
            DiagAction::Wait(duration) => {
                println!("wait {} ms", duration.as_millis());
                std::thread::sleep(*duration);
            }
        }
        run(Vec::new(), false);
    }
    run(Vec::new(), true);
}

/// A `;`-separated input script for the diagnostic verbs: `x,y` clicks,
/// `+text` types into whatever the preceding click focused, `~ms` waits.
fn parse_script(spec: &str) -> Vec<DiagAction> {
    spec.split(';')
        .filter_map(|s| {
            if let Some(text) = s.strip_prefix('+') {
                return Some(DiagAction::Type(text.to_string()));
            }
            if let Some(ms) = s.trim().strip_prefix('~') {
                return Some(DiagAction::Wait(std::time::Duration::from_millis(ms.parse().ok()?)));
            }
            let (x, y) = s.split_once(',')?;
            Some(DiagAction::Click(egui::pos2(x.trim().parse().ok()?, y.trim().parse().ok()?)))
        })
        .collect()
}

fn screen_size(arg: Option<String>, default: egui::Vec2) -> egui::Vec2 {
    arg.and_then(|s| {
        let (w, h) = s.split_once('x')?;
        Some(egui::vec2(w.parse().ok()?, h.parse().ok()?))
    })
    .unwrap_or(default)
}

fn main() -> eframe::Result {
    let mut args = std::env::args().skip(1);
    let verb = args.next();
    // Every verb that needs no toolkit is `launcher_core::cli`, so this
    // binary and `launcher-qt` answer them with the same code.
    if let Some(verb) = verb.as_deref() {
        if let Some(code) = cli::run(verb, &mut args) {
            std::process::exit(code);
        }
    }
    match verb.as_deref() {
        Some("--pick-file") => {
            // Exercises the real OS file dialog (rfd) headlessly — proof
            // the portal/NSOpenPanel/IFileDialog wiring works, since this
            // session has no GUI click automation to drive the wizard's
            // "Browse…" button through an actual dialog. An optional arg
            // (a path field's current value, file or directory) exercises
            // the same start-directory extraction `path_field` uses. No
            // Qt twin: that build's dialog is declarative, in QML.
            let start_dir = args.next().and_then(|v| filepicker::start_dir(&v));
            match filepicker::pick_file_headless(None, start_dir.as_deref()) {
                Some(path) => println!("{}", path.display()),
                None => println!("(cancelled)"),
            }
            return Ok(());
        }
        Some("--diag-preview-frame") => {
            // Diagnostic only: renders one full egui frame containing
            // just `ui.image()` on the preview's texture, the way
            // eframe's own paint step would, and dumps the *composited*
            // result — unlike `--preview-shader`, which reads the
            // shader's own output texture directly and so can't see a
            // bug in how that texture is displayed through egui (exactly
            // what a report of "the preview shows solid black" needs to
            // rule in or out).
            let usage = "usage: launcher --diag-preview-frame <preset.slangp> <image> <out.png>";
            let preset: PathBuf = args.next().expect(usage).into();
            let image_path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let (aw, ah) = cli::preview_area_env();
            let render_state = headless_render_state();
            let mut preview = shader_preview::Preview::new(render_state.clone());
            preview.update(&preset, &[], &image_path, egui::vec2(aw as f32, ah as f32));
            if let Some(err) = preview.error() {
                eprintln!("[preview] {err}");
            }
            let tex_id = preview.texture_id().expect("preview never registered a texture");
            let size = preview.viewport_size();

            let ctx = egui::Context::default();
            let full_output = ctx.run_ui(egui::RawInput::default(), |ui| {
                ui.image((tex_id, size));
            });
            dump_egui_frame(&render_state, &ctx, full_output, [800, 600], &out);
            return Ok(());
        }
        Some("--diag-snapshots-frame") => {
            // Diagnostic only: the real snapshot window through egui
            // headlessly with synthetic clicks, like --diag-shelf-frame.
            let usage =
                "usage: launcher --diag-snapshots-frame <machine.toml> <out.png> [<screen WxH>] [<script: x,y click; +text type>] [running]";
            let path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let screen = screen_size(args.next(), egui::vec2(900.0, 600.0));
            let script = parse_script(&args.next().unwrap_or_default());
            let running = args.next().as_deref() == Some("running");

            let mut window = snapshots_ui::SnapshotWindow::default();
            window.model.open_for_path(&path, running);
            let render_state = headless_render_state();
            diag_window_frames(&render_state, screen, &script, &out, |ctx| window.show(ctx, running));
            // A click that started a live snapshot job leaves it in
            // flight: the frames here run back to back, where the real
            // window would poll it over its repaint tick.
            window.model.wait_for_job(std::time::Duration::from_secs(120));
            cli::print_snapshots(&window.model);
            return Ok(());
        }
        Some("--diag-wizard-frame") => {
            // The same, for the machine form: `new <win98|xp|dos>` opens
            // it as "New machine", `edit <machine.toml>` as "Edit
            // machine". The dump is how the memory, processor and
            // acceleration rows are checked for real — they render per
            // family (a DOS machine opens on 64 MB and a 486DX2-66, and
            // says out loud that a chosen processor means emulation) and
            // the acceleration hint depends on this host — and a click
            // script drives them.
            let usage =
                "usage: launcher --diag-wizard-frame new <win98|xp|dos> | edit <machine.toml> -- <out.png> [<screen WxH>] [<script>]";
            let mode = args.next().expect(usage);
            let arg = args.next().expect(usage);
            let mut form = launcher_core::wizard::Form::default();
            match mode.as_str() {
                "new" => form.open_new(cli::parse_family(Some(arg.as_str()), usage)),
                "edit" => form.open_edit_path(arg.into()),
                _ => panic!("{usage}"),
            }
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            let out = args.next().expect(usage);
            let screen = screen_size(args.next(), egui::vec2(700.0, 600.0));
            let script = parse_script(&args.next().unwrap_or_default());
            let library_dir = library::default_dir();
            let profiles = shader_library::scan(&shader_library::default_dir());
            let render_state = headless_render_state();
            diag_window_frames(&render_state, screen, &script, &out, |ctx| {
                if let Some(saved) = wizard::show(&mut form, ctx, &library_dir, &profiles) {
                    println!("saved {}", saved.display());
                }
            });
            return Ok(());
        }
        Some("--diag-shelf-frame") => {
            // Diagnostic only: runs the *real* disc-shelf window through
            // egui headlessly, applies synthetic clicks (Boot/Insert/
            // Remove/Add — read their positions off a dump with no
            // clicks first), dumps the composited frame and prints the
            // resulting shelf. That's what proves the buttons are wired
            // to the row they appear next to, in a session with no GUI
            // click automation to try it by hand.
            let usage =
                "usage: launcher --diag-shelf-frame <machine.toml|shelf> <out.png> [<screen WxH>] [<script: x,y click; +text type>] [running]";
            let path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let screen = screen_size(args.next(), egui::vec2(900.0, 600.0));
            let script = parse_script(&args.next().unwrap_or_default());
            let running = args.next().as_deref() == Some("running");

            let mut window = discshelf::DiscShelfWindow::default();
            // `shelf` in place of a bundle opens the window on the
            // shared shelf alone, the way the bottom row's button does.
            if path == Path::new("shelf") {
                window.shelf.open_library(&disc_library::default_path());
            } else {
                window.shelf.open_for_path(path, &disc_library::default_path());
            }
            let render_state = headless_render_state();
            diag_window_frames(&render_state, screen, &script, &out, |ctx| {
                if let Some(saved) = window.show(ctx, running) {
                    println!("saved {}", saved.display());
                }
            });
            for disc in window.shelf.discs() {
                println!("{}\t{}", disc.label, disc.path.display());
            }
            return Ok(());
        }
        Some("--diag-editor-frame") => {
            // Diagnostic only: runs the *real* shader-profile editor
            // window through egui headlessly — including a synthetic
            // drag of its bottom edge and synthetic clicks at given
            // screen positions (the "Fullscreen" checkbox, say) — and
            // dumps the composited frame, plus the window's rect per
            // frame. That's what proves the window resizes vertically
            // and that its body (sliders and preview) grows with it.
            let usage =
                "usage: launcher --diag-editor-frame <preset.slangp> <image> <out.png> [<screen WxH>] [<drag dy>] [<x,y;x,y clicks>]";
            let preset = args.next().expect(usage);
            let image = args.next().expect(usage);
            let out = args.next().expect(usage);
            let screen = screen_size(args.next(), egui::vec2(1400.0, 900.0));
            let drag_dy: f32 = args.next().and_then(|s| s.parse().ok()).unwrap_or(0.0);
            // This one drives its own frames (it also drags the window's
            // bottom edge and prints the rect per frame), so it runs the
            // shared script's steps itself.
            let steps = parse_script(&args.next().unwrap_or_default());

            let render_state = headless_render_state();
            let profiles_dir = shader_library::default_dir();
            let ctx = egui::Context::default();
            let mut manager = shader_manager::ShaderManager::default();
            if preset == "list" {
                manager.open_list(); // the profile list rather than the editor
            } else {
                manager.debug_open_editor(preset, image, false);
            }
            let screen_rect = egui::Rect::from_min_size(egui::Pos2::ZERO, screen);

            // One frame per step; only the last one is painted, but every
            // frame's texture deltas are applied (see `apply_texture_deltas`).
            let mut run = |events: Vec<egui::Event>, paint: bool| {
                let input = egui::RawInput { screen_rect: Some(screen_rect), events, ..Default::default() };
                let mut full_output =
                    ctx.run_ui(input, |ui| _ = manager.show(ui.ctx(), &profiles_dir, Some(&render_state)));
                let rect = manager.windowed_rect().unwrap_or(egui::Rect::NOTHING);
                println!("window {:.0}x{:.0} at {:.0},{:.0}", rect.width(), rect.height(), rect.min.x, rect.min.y);
                if paint {
                    dump_egui_frame(&render_state, &ctx, full_output, [screen.x as u32, screen.y as u32], &out);
                } else {
                    apply_texture_deltas(&render_state, &mut full_output.textures_delta);
                }
                rect
            };

            let button = |pos, pressed| egui::Event::PointerButton {
                pos,
                button: egui::PointerButton::Primary,
                pressed,
                modifiers: egui::Modifiers::default(),
            };

            run(Vec::new(), false);
            let rect = run(Vec::new(), false); // settle: egui needs a frame to lay a new window out
            if drag_dy != 0.0 {
                // Grab the bottom edge and pull: hover, press, move, release.
                let grab = rect.center_bottom();
                let to = grab + egui::vec2(0.0, drag_dy);
                run(vec![egui::Event::PointerMoved(grab)], false);
                run(vec![button(grab, true)], false);
                run(vec![egui::Event::PointerMoved(to)], false);
                run(vec![egui::Event::PointerMoved(to)], false);
                run(vec![button(to, false)], false);
                run(vec![egui::Event::PointerGone], false);
            }
            for step in steps {
                match step {
                    DiagAction::Click(pos) => {
                        println!("click at {:.0},{:.0}", pos.x, pos.y);
                        run(vec![egui::Event::PointerMoved(pos)], false);
                        run(vec![button(pos, true)], false);
                        run(vec![button(pos, false)], false);
                        run(vec![egui::Event::PointerGone], false);
                    }
                    DiagAction::Wait(duration) => {
                        println!("wait {} ms", duration.as_millis());
                        std::thread::sleep(duration);
                    }
                    DiagAction::Type(_) => {} // this window has nothing to type into
                }
                run(Vec::new(), false);
            }
            run(Vec::new(), false);
            run(Vec::new(), true);
            return Ok(());
        }
        Some(other) if other.starts_with("--") => {
            eprintln!("unknown option {other}");
            std::process::exit(2);
        }
        _ => {}
    }

    let machines = Machines::load();
    // Debug hook: screenshot the real windowed editor (bypassing the
    // GUI click this session has no automation for) pre-filled from
    // `LAUNCHER_DEBUG_SHADER_PREVIEW=<preset.slangp>;<image>[;fullscreen]`.
    let debug_shader_preview = std::env::var("LAUNCHER_DEBUG_SHADER_PREVIEW").ok();
    // The window's own identity, which only matters once this is
    // installed (M6 step 6): `app_id` is what a Wayland compositor matches
    // against `com._2ksbox.Launcher.desktop` to give the window its icon
    // and its name in a task switcher, and `icon` is the same picture
    // handed over directly, for X11 and Windows where there is no such
    // matching. The PNG is rendered from the very SVG the desktop entry
    // points at, so the two can't drift apart.
    let icon = {
        let png = include_bytes!("../../packaging/linux/com._2ksbox.Launcher-128.png");
        image::load_from_memory(png).map(|img| {
            let rgba = img.to_rgba8();
            let (width, height) = rgba.dimensions();
            egui::IconData { rgba: rgba.into_raw(), width, height }
        })
    };
    let mut viewport = egui::ViewportBuilder::default().with_app_id(paths::APP_ID);
    match icon {
        Ok(icon) => viewport = viewport.with_icon(icon),
        // A broken icon is not a reason to refuse to start.
        Err(e) => eprintln!("[launcher] window icon: {e}"),
    }
    eframe::run_native(
        paths::NAME,
        eframe::NativeOptions { viewport, ..Default::default() },
        Box::new(|cc| {
            let mut shader_manager = shader_manager::ShaderManager::default();
            if let Some(spec) = debug_shader_preview {
                let mut parts = spec.split(';');
                if let (Some(preset), Some(image)) = (parts.next(), parts.next()) {
                    let fullscreen = parts.next() == Some("fullscreen");
                    shader_manager.debug_open_editor(preset.to_string(), image.to_string(), fullscreen);
                }
            }
            Ok(Box::new(LauncherApp {
                machines,
                wizard: launcher_core::wizard::Form::default(),
                disc_shelf: discshelf::DiscShelfWindow::default(),
                snapshots: snapshots_ui::SnapshotWindow::default(),
                shader_manager,
                wgpu_render_state: cc.wgpu_render_state.clone(),
            }))
        }),
    )
}
