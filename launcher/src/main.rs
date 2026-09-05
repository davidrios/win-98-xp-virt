//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: the library grid (`library.rs`) can spawn a player
//! (`player.rs`) and create or edit machines through a wizard
//! (`wizard.rs`) over the `machine.toml` bundle format (`bundle.rs`); no
//! thumbnails yet.

mod bundle;
mod control;
mod discshelf;
mod filepicker;
mod library;
mod player;
mod shader_library;
mod shader_manager;
mod shader_preview;
mod shader_profile;
mod snapshots;
mod wizard;

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Child;

struct LauncherApp {
    library_dir: PathBuf,
    entries: Vec<library::LibraryEntry>,
    shader_profiles_dir: PathBuf,
    shader_profiles: Vec<shader_library::ProfileEntry>,
    /// Bundle directory -> its player process, while running. A bundle's
    /// absence here means "not running" (never tracked as ended-but-kept:
    /// `try_wait` removes it below the moment it exits).
    running: HashMap<PathBuf, Child>,
    wizard: wizard::Wizard,
    disc_shelf: discshelf::DiscShelf,
    snapshots: snapshots::SnapshotWindow,
    shader_manager: shader_manager::ShaderManager,
    /// `None` on a non-wgpu eframe backend (not expected in practice —
    /// `wgpu` is a default feature, see `docs/tracks/m6-launcher.md` —
    /// but the shader profile editor degrades to "no live preview"
    /// rather than unwrapping this).
    wgpu_render_state: Option<eframe::egui_wgpu::RenderState>,
}

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        // No push notification from a child exiting: poll for it instead.
        ctx.request_repaint_after(std::time::Duration::from_millis(500));
        self.running.retain(|dir, child| match child.try_wait() {
            Ok(None) => true,
            Ok(Some(status)) => {
                eprintln!("[launcher] {} exited: {status}", dir.display());
                false
            }
            Err(e) => {
                eprintln!("[launcher] {}: {e}", dir.display());
                false
            }
        });

        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading("win98-xp-virt");
            ui.add_space(8.0);
            if self.entries.is_empty() {
                ui.label("No machines yet.");
                ui.label(format!("Library: {}", self.library_dir.display()));
            } else {
                egui::Grid::new("library").striped(true).show(ui, |ui| {
                    ui.strong("Name");
                    ui.strong("Family");
                    ui.strong("Shader");
                    ui.strong("Location");
                    ui.strong("");
                    ui.end_row();
                    for entry in &self.entries {
                        ui.label(&entry.machine.name);
                        ui.label(match entry.machine.family {
                            bundle::Family::Win98 => "Win98",
                            bundle::Family::Xp => "XP",
                        });
                        let shader_label = entry
                            .machine
                            .shader_profile
                            .as_deref()
                            .and_then(|id| self.shader_profiles.iter().find(|e| shader_library::id_of(&e.path) == id))
                            .map(|e| e.profile.name.clone())
                            .or_else(|| entry.machine.shader.as_ref().map(|p| p.display().to_string()))
                            .unwrap_or_else(|| "(default)".to_string());
                        ui.label(shader_label);
                        ui.label(entry.dir.display().to_string());
                        ui.horizontal(|ui| {
                            if self.running.contains_key(&entry.dir) {
                                ui.label("Running");
                            } else if ui.button("Play").clicked() {
                                // The monitor socket is derived from the
                                // bundle directory, so every window that
                                // wants live control can find it again
                                // without the app carrying it around.
                                let socket = control::socket_path(&entry.dir);
                                match player::spawn(&entry.machine, Some(&socket)) {
                                    Ok(child) => {
                                        self.running.insert(entry.dir.clone(), child);
                                    }
                                    Err(e) => {
                                        eprintln!("[launcher] spawning player for {}: {e}", entry.dir.display())
                                    }
                                }
                            }
                            if ui.button("Edit…").clicked() {
                                self.wizard.open_edit(&entry.machine, entry.dir.join(library::BUNDLE_FILE));
                            }
                            let discs = entry.machine.discs.len();
                            if ui.button(format!("Discs ({discs})…")).clicked() {
                                self.disc_shelf.open_for(&entry.machine, entry.dir.join(library::BUNDLE_FILE));
                            }
                            if ui.button("Snapshots…").clicked() {
                                let running = self.running.contains_key(&entry.dir);
                                self.snapshots.open_for(&entry.machine, entry.dir.clone(), running);
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
                if ui.button("Shader profiles…").clicked() {
                    self.shader_manager.open_list();
                }
            });
        });

        if let Some(_bundle_path) = self.wizard.show(&ctx, &self.library_dir, &self.shader_profiles) {
            self.entries = library::scan(&self.library_dir);
        }
        // The shelf window is per-machine but the app owns one: whether
        // *that* machine is running decides the "applies to the next
        // boot" note, so look it up by the bundle it has open.
        let shelf_running = self
            .disc_shelf
            .bundle_dir()
            .map(|dir| self.running.contains_key(dir))
            .unwrap_or(false);
        if self.disc_shelf.show(&ctx, shelf_running).is_some() {
            self.entries = library::scan(&self.library_dir);
        }
        let snapshots_running =
            self.snapshots.bundle_dir().map(|dir| self.running.contains_key(dir)).unwrap_or(false);
        self.snapshots.show(&ctx, snapshots_running);
        if self
            .shader_manager
            .show(&ctx, &self.shader_profiles_dir, self.wgpu_render_state.as_ref())
            .is_some()
        {
            self.shader_profiles = shader_library::scan(&self.shader_profiles_dir);
        }
    }
}

/// `PREVIEW_AREA=<w>x<h>` for the shader-preview debug verbs — the area
/// the real editor's preview pane would have reserved, since headlessly
/// there's no window to measure one from. Defaults to 800x600, a
/// plausible non-fullscreen editor size.
fn preview_area_env() -> egui::Vec2 {
    std::env::var("PREVIEW_AREA")
        .ok()
        .and_then(|s| {
            let (w, h) = s.split_once('x')?;
            Some(egui::Vec2::new(w.parse().ok()?, h.parse().ok()?))
        })
        .unwrap_or(egui::Vec2::new(800.0, 600.0))
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
}

/// Drives one of the app's windows through egui headlessly with
/// synthetic input and dumps the composited frame — the same trick
/// `--diag-editor-frame` uses, generalized so a window whose whole
/// content is buttons and fields (the disc shelf, the snapshot list) can
/// have its wiring exercised in a session with no GUI click automation.
/// Each action gets its own frames, because egui needs a frame to notice
/// a widget was pressed and another to react.
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
        }
        run(Vec::new(), false);
    }
    run(Vec::new(), true);
}

/// A `;`-separated input script for the diagnostic verbs: `x,y` clicks,
/// `+text` types into whatever the preceding click focused.
fn parse_script(spec: &str) -> Vec<DiagAction> {
    spec.split(';')
        .filter_map(|s| {
            if let Some(text) = s.strip_prefix('+') {
                return Some(DiagAction::Type(text.to_string()));
            }
            let (x, y) = s.split_once(',')?;
            Some(DiagAction::Click(egui::pos2(x.trim().parse().ok()?, y.trim().parse().ok()?)))
        })
        .collect()
}

fn main() -> eframe::Result {
    // Debug/advanced-drawer aids (doc 07), until the wizard exists:
    // `--new` bootstraps a bundle into the library from the doc 06
    // reference defaults, `--print-args` shows the qemu-system-i386
    // command line a bundle translates to. Neither opens a window.
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("--print-args") => {
            let path = args.next().expect("usage: launcher --print-args <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            println!("{}", machine.qemu_args(&player::pc_bios_dir()).join(" "));
            return Ok(());
        }
        Some("--play") => {
            let path = PathBuf::from(args.next().expect("usage: launcher --play <machine.toml>"));
            let machine = bundle::Machine::load(&path).expect("load bundle");
            // Same monitor socket the grid's "Play" opens, so the live
            // half of `--disc-shelf`/`--snapshots` can be scripted
            // against a player started this way.
            let socket = control::socket_path(path.parent().unwrap_or(std::path::Path::new(".")));
            let child = player::spawn(&machine, Some(&socket)).expect("spawn player");
            println!("pid {} qmp {}", child.id(), socket.display());
            return Ok(());
        }
        Some("--pick-file") => {
            // Exercises the real OS file dialog (rfd) headlessly — proof
            // the portal/NSOpenPanel/IFileDialog wiring works, since this
            // session has no GUI click automation to drive the wizard's
            // "Browse…" button through an actual dialog. An optional arg
            // (a path field's current value, file or directory) exercises
            // the same start-directory extraction path_field itself uses.
            let start_dir = args.next().and_then(|v| filepicker::start_dir(&v));
            match filepicker::pick_file_headless(None, start_dir.as_deref()) {
                Some(path) => println!("{}", path.display()),
                None => println!("(cancelled)"),
            }
            return Ok(());
        }
        Some("--wizard-new") => {
            // Headless equivalent of the "New machine" window, for
            // scripted testing of the wizard's actual submit() logic
            // (disk creation via qemu-img included) without a GUI click.
            let usage = "usage: launcher --wizard-new <win98|xp> <name> <disk-size-gb>";
            let family = match args.next().as_deref() {
                Some("win98") => bundle::Family::Win98,
                Some("xp") => bundle::Family::Xp,
                _ => panic!("{usage}"),
            };
            let name = args.next().expect(usage);
            let size_gb: u32 = args.next().expect(usage).parse().expect("disk size must be a number");
            let w = wizard::Wizard::with_new_disk(family, name, size_gb);
            let path = w.submit(&library::default_dir()).expect("create bundle");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--wizard-edit") => {
            // Headless equivalent of clicking "Edit…" then "Save": loads
            // a bundle, renames it, saves it back in place, without a
            // GUI click.
            let usage = "usage: launcher --wizard-edit <machine.toml> <new-name>";
            let path = args.next().expect(usage);
            let new_name = args.next().expect(usage);
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            let mut w = wizard::Wizard::default();
            w.open_edit(&machine, path.clone().into());
            w.set_name(new_name);
            let saved = w.submit(&library::default_dir()).expect("save bundle");
            println!("{}", saved.display());
            return Ok(());
        }
        Some("--disc-shelf") => {
            // Headless equivalent of the "Discs…" window: with no discs
            // given it prints the current shelf, otherwise it sets the
            // shelf to the given paths (in order — the first is the boot
            // disc) and calls the same `save()` the "Save" button does.
            // `+tools` stands for the "Add guest-tools ISO" button.
            let usage = "usage: launcher --disc-shelf <machine.toml> [<disc>|+tools ...]";
            let path: PathBuf = args.next().expect(usage).into();
            let machine = bundle::Machine::load(&path).expect("load bundle");
            let mut shelf = discshelf::DiscShelf::default();
            shelf.open_for(&machine, path.clone());
            let discs: Vec<PathBuf> = args
                .map(|a| match a.as_str() {
                    "+tools" => discshelf::guest_tools_iso().expect("no guest-tools ISO built"),
                    other => other.into(),
                })
                .collect();
            if !discs.is_empty() {
                shelf.set_discs(discs);
                shelf.save().expect("save shelf");
            }
            for (i, disc) in shelf.discs().iter().enumerate() {
                println!("{}{}", if i == 0 { "boot " } else { "     " }, disc.display());
            }
            return Ok(());
        }
        Some("--new") => {
            let usage = "usage: launcher --new <win98|xp> <name> <disk.qcow2>";
            let family = match args.next().as_deref() {
                Some("win98") => bundle::Family::Win98,
                Some("xp") => bundle::Family::Xp,
                _ => panic!("{usage}"),
            };
            let name = args.next().expect(usage);
            let disk = args.next().expect(usage).into();
            let path = library::create(&library::default_dir(), family, name, disk).expect("create bundle");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--new-shader-profile") => {
            let usage = "usage: launcher --new-shader-profile <name> <preset.slangp>";
            let name = args.next().expect(usage);
            let preset = args.next().expect(usage).into();
            let path = shader_library::create(&shader_library::default_dir(), name, preset).expect("create profile");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--set-shader-param") => {
            let usage = "usage: launcher --set-shader-param <profile.toml> <param> <value>";
            let path: PathBuf = args.next().expect(usage).into();
            let param = args.next().expect(usage);
            let value: f32 = args.next().expect(usage).parse().expect("value must be a number");
            let mut profile = shader_profile::ShaderProfile::load(&path).expect("load profile");
            profile.params.insert(param, value);
            profile.save(&path).expect("save profile");
            println!("{}", profile.params_arg().unwrap_or_default());
            return Ok(());
        }
        Some("--list-shader-params") => {
            let preset = args.next().expect("usage: launcher --list-shader-params <preset.slangp>");
            let params = shader_profile::parameter_meta(std::path::Path::new(&preset)).expect("parse preset");
            for p in params {
                println!("{} [{}..{}] step {} = {} — {}", p.id, p.minimum, p.maximum, p.step, p.default, p.description);
            }
            return Ok(());
        }
        Some("--assign-shader") => {
            let usage = "usage: launcher --assign-shader <machine.toml> <profile-id-or-(none)>";
            let path: PathBuf = args.next().expect(usage).into();
            let id = args.next().expect(usage);
            let mut machine = bundle::Machine::load(&path).expect("load bundle");
            machine.shader_profile = if id == "(none)" { None } else { Some(id) };
            machine.save(&path).expect("save bundle");
            return Ok(());
        }
        Some("--print-shader-args") => {
            let path = args.next().expect("usage: launcher --print-shader-args <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            println!("{}", player::shader_args(&machine).join(" "));
            return Ok(());
        }
        Some("--preview-shader") => {
            // Headless equivalent of the shader manager's live preview
            // pane: proves `shader_preview::Preview`'s image-decode and
            // render path (not just `shader-chain`, already exercised by
            // the player) without a GUI click or a visible window — a
            // real (if windowless) wgpu adapter/device via
            // `egui_wgpu::RenderState::create`, same as eframe itself
            // uses at startup.
            let usage = "usage: launcher --preview-shader <preset.slangp> <image> <out.png> [name=value,...]";
            let preset: PathBuf = args.next().expect(usage).into();
            let image_path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let params = shader_profile::parse_params(&args.next().unwrap_or_default());
            let area = preview_area_env(); // PREVIEW_AREA=WxH, default 800x600
            let instance = eframe::wgpu::Instance::new(
                eframe::wgpu::InstanceDescriptor::new_without_display_handle_from_env(),
            );
            let render_state = pollster::block_on(eframe::egui_wgpu::RenderState::create(
                &eframe::egui_wgpu::WgpuConfiguration::default(),
                &instance,
                None,
                eframe::egui_wgpu::RendererOptions::default(),
            ))
            .expect("create a headless wgpu render state");
            let mut preview = shader_preview::Preview::new(render_state.clone());
            preview.update(&preset, &params, &image_path, area);
            if let Some(err) = preview.error() {
                eprintln!("[preview] {err}");
            }
            let tex = preview.output_texture().expect("no frame rendered");
            shader_chain::dump_texture(&render_state.device, &render_state.queue, tex, &out);
            return Ok(());
        }
        Some("--diag-preview-frame") => {
            // Diagnostic only (not a documented debug verb): renders one
            // full egui frame containing just `ui.image()` on the
            // preview's texture, the same way eframe's own paint step
            // would, and dumps the *composited* result — unlike
            // `--preview-shader`, which reads the shader's own output
            // texture directly and so can't see a bug in how that
            // texture is displayed through egui (exactly what a report
            // of "the preview shows solid black" needs to rule in or out).
            let usage = "usage: launcher --diag-preview-frame <preset.slangp> <image> <out.png>";
            let preset: PathBuf = args.next().expect(usage).into();
            let image_path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let area = preview_area_env();
            let render_state = headless_render_state();
            let mut preview = shader_preview::Preview::new(render_state.clone());
            preview.update(&preset, &[], &image_path, area);
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
        Some("--qmp-socket") => {
            // Where a bundle's live-control monitor socket lives, so a
            // script (or a hand-run QEMU standing in for the player) can
            // put one there.
            let path = PathBuf::from(args.next().expect("usage: launcher --qmp-socket <machine.toml>"));
            println!("{}", control::socket_path(path.parent().unwrap_or(std::path::Path::new("."))).display());
            return Ok(());
        }
        Some("--insert-disc") => {
            // Headless equivalent of the disc shelf's live "Insert" /
            // "Eject" buttons: the same `DiscShelf` calls, against the
            // machine's running monitor.
            let usage = "usage: launcher --insert-disc <machine.toml> <disc|eject>";
            let path: PathBuf = args.next().expect(usage).into();
            let machine = bundle::Machine::load(&path).expect("load bundle");
            let what = args.next().expect(usage);
            let mut shelf = discshelf::DiscShelf::default();
            shelf.open_for(&machine, path);
            if what == "eject" {
                shelf.eject_live();
            } else {
                shelf.insert_live(std::path::Path::new(&what));
            }
            match shelf.last_result() {
                Ok(status) => println!("{}", status.unwrap_or("(nothing happened)")),
                Err(e) => {
                    eprintln!("[disc-shelf] {e}");
                    std::process::exit(1);
                }
            }
            return Ok(());
        }
        Some("--snapshots") => {
            // Headless equivalent of the "Snapshots…" window: the same
            // `SnapshotWindow` operations its buttons run, over a
            // bundle's disk, without a GUI click.
            let usage = "usage: launcher --snapshots [--live] <machine.toml> [take|delete|restore <name>]";
            let mut next = args.next();
            // `--live` drives a *running* machine's monitor instead of
            // qemu-img, the way the window does when its player is up.
            let live = next.as_deref() == Some("--live");
            if live {
                next = args.next();
            }
            let path: PathBuf = next.expect(usage).into();
            let machine = bundle::Machine::load(&path).expect("load bundle");
            let dir = path.parent().unwrap_or(std::path::Path::new(".")).to_path_buf();
            let mut window = snapshots::SnapshotWindow::default();
            window.open_for(&machine, dir, live);
            match (args.next().as_deref(), args.next()) {
                (Some("take"), Some(name)) => window.take(&name),
                (Some("delete"), Some(name)) => window.drop_snapshot(&name),
                (Some("restore"), Some(name)) => window.revert(&name),
                (None, _) => {}
                _ => panic!("{usage}"),
            }
            // A live operation is a QMP *job*: it returns as soon as the
            // job exists and finishes later, so wait for it here the way
            // the window's repaint tick does.
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
            while window.job_pending() && std::time::Instant::now() < deadline {
                std::thread::sleep(std::time::Duration::from_millis(200));
                window.poll_job_now();
            }
            if let Some(status) = window.status() {
                println!("[snapshots] {status}");
            }
            if let Some(err) = window.error() {
                eprintln!("[snapshots] {err}");
            }
            for snap in window.snapshots() {
                println!("{}\t{}\t{}\t{}", snap.id, snap.name, snap.date_label(), snap.size_label());
            }
            return Ok(());
        }
        Some("--diag-snapshots-frame") => {
            // Diagnostic only: the real snapshot window through egui
            // headlessly with synthetic clicks, like --diag-shelf-frame.
            let usage =
                "usage: launcher --diag-snapshots-frame <machine.toml> <out.png> [<screen WxH>] [<script: x,y click; +text type>] [running]";
            let path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let screen = args
                .next()
                .and_then(|s| {
                    let (w, h) = s.split_once('x')?;
                    Some(egui::vec2(w.parse().ok()?, h.parse().ok()?))
                })
                .unwrap_or(egui::vec2(900.0, 600.0));
            let script = parse_script(&args.next().unwrap_or_default());
            let running = args.next().as_deref() == Some("running");

            let machine = bundle::Machine::load(&path).expect("load bundle");
            let dir = path.parent().unwrap_or(std::path::Path::new(".")).to_path_buf();
            let mut window = snapshots::SnapshotWindow::default();
            window.open_for(&machine, dir, running);
            let render_state = headless_render_state();
            diag_window_frames(&render_state, screen, &script, &out, |ctx| window.show(ctx, running));
            // A click that started a live snapshot job leaves it in
            // flight: the frames here run back to back, where the real
            // window would poll it over its repaint tick.
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
            while window.job_pending() && std::time::Instant::now() < deadline {
                std::thread::sleep(std::time::Duration::from_millis(200));
                window.poll_job_now();
            }
            if let Some(status) = window.status() {
                println!("[snapshots] {status}");
            }
            if let Some(err) = window.error() {
                eprintln!("[snapshots] {err}");
            }
            for snap in window.snapshots() {
                println!("{}\t{}\t{}\t{}", snap.id, snap.name, snap.date_label(), snap.size_label());
            }
            return Ok(());
        }
        Some("--diag-shelf-frame") => {
            // Diagnostic only: runs the *real* disc-shelf window through
            // egui headlessly, applies synthetic clicks (↑/↓/Remove/Add/
            // Save — read their positions off a dump with no clicks
            // first), dumps the composited frame and prints the resulting
            // shelf. That's what proves the buttons are wired to the list
            // they appear next to, in a session with no GUI click
            // automation to try it by hand.
            let usage =
                "usage: launcher --diag-shelf-frame <machine.toml> <out.png> [<screen WxH>] [<script: x,y click; +text type>] [running]";
            let path: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let screen = args
                .next()
                .and_then(|s| {
                    let (w, h) = s.split_once('x')?;
                    Some(egui::vec2(w.parse().ok()?, h.parse().ok()?))
                })
                .unwrap_or(egui::vec2(900.0, 600.0));
            let script = parse_script(&args.next().unwrap_or_default());
            let running = args.next().as_deref() == Some("running");

            let machine = bundle::Machine::load(&path).expect("load bundle");
            let mut shelf = discshelf::DiscShelf::default();
            shelf.open_for(&machine, path);
            let render_state = headless_render_state();
            diag_window_frames(&render_state, screen, &script, &out, |ctx| {
                if let Some(saved) = shelf.show(ctx, running) {
                    println!("saved {}", saved.display());
                }
            });
            for (i, disc) in shelf.discs().iter().enumerate() {
                println!("{}{}", if i == 0 { "boot " } else { "     " }, disc.display());
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
            // and that its body (sliders and preview) grows with it, in
            // a session with no GUI click automation to try it by hand.
            let usage =
                "usage: launcher --diag-editor-frame <preset.slangp> <image> <out.png> [<screen WxH>] [<drag dy>] [<x,y;x,y clicks>]";
            let preset = args.next().expect(usage);
            let image = args.next().expect(usage);
            let out = args.next().expect(usage);
            let screen = args
                .next()
                .and_then(|s| {
                    let (w, h) = s.split_once('x')?;
                    Some(egui::vec2(w.parse().ok()?, h.parse().ok()?))
                })
                .unwrap_or(egui::vec2(1400.0, 900.0));
            let drag_dy: f32 = args.next().and_then(|s| s.parse().ok()).unwrap_or(0.0);
            // This one drives its own frames (it also drags the window's
            // bottom edge and prints the rect per frame), so it takes
            // just the click positions out of the shared script parser.
            let clicks: Vec<egui::Pos2> = parse_script(&args.next().unwrap_or_default())
                .into_iter()
                .filter_map(|a| match a {
                    DiagAction::Click(pos) => Some(pos),
                    DiagAction::Type(_) => None,
                })
                .collect();

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
            for pos in clicks {
                println!("click at {:.0},{:.0}", pos.x, pos.y);
                run(vec![egui::Event::PointerMoved(pos)], false);
                run(vec![button(pos, true)], false);
                run(vec![button(pos, false)], false);
                run(vec![egui::Event::PointerGone], false);
                run(Vec::new(), false);
            }
            run(Vec::new(), false);
            run(Vec::new(), true);
            return Ok(());
        }
        _ => {}
    }

    let library_dir = library::default_dir();
    let entries = library::scan(&library_dir);
    let shader_profiles_dir = shader_library::default_dir();
    let shader_profiles = shader_library::scan(&shader_profiles_dir);
    // Debug hook: screenshot the real windowed editor (bypassing the
    // GUI click this session has no automation for) pre-filled from
    // `LAUNCHER_DEBUG_SHADER_PREVIEW=<preset.slangp>;<image>[;fullscreen]`.
    let debug_shader_preview = std::env::var("LAUNCHER_DEBUG_SHADER_PREVIEW").ok();
    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
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
                library_dir,
                entries,
                shader_profiles_dir,
                shader_profiles,
                running: HashMap::new(),
                wizard: wizard::Wizard::default(),
                disc_shelf: discshelf::DiscShelf::default(),
                snapshots: snapshots::SnapshotWindow::default(),
                shader_manager,
                wgpu_render_state: cc.wgpu_render_state.clone(),
            }))
        }),
    )
}
