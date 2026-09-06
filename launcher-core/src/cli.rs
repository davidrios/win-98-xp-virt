//! Every debug verb that needs no toolkit, in one place, so both front
//! ends answer the same ones with the same code (doc 07, and the README
//! table). They are how the launcher is tested at all — CLAUDE.md's
//! policy is integration and end-to-end only, and a verb here drives the
//! real model a button drives, without a GUI click.
//!
//! `run` returns `Some(exit code)` when it recognised the verb, `None`
//! when the caller should keep looking (its own toolkit-bound verbs) or
//! open a window. Both binaries call it first thing, before a GUI exists.
//!
//! Two verbs are deliberately *not* here, because they are the toolkit:
//! `launcher --pick-file` pops the real `rfd` dialog (Qt's own dialog is
//! declarative, in QML, and has nothing to call), and the `--diag-*`
//! screenshot verbs render real frames — synthetic egui input on one
//! side, `QT_QPA_PLATFORM=offscreen` and `grabToImage` on the other.

use crate::bundle::{self, Family, Machine, Optimization};
use crate::{browse, control, disc_library, library, machines, player, preview, shader_library, shader_profile,
    shader_source, shelf, snaps, wizard};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// `PREVIEW_AREA=<w>x<h>`: the area the real editor's preview pane would
/// have reserved, since headlessly there is no window to measure one
/// from. 800x600 is a plausible non-fullscreen editor.
pub fn preview_area_env() -> (u32, u32) {
    std::env::var("PREVIEW_AREA")
        .ok()
        .and_then(|s| {
            let (w, h) = s.split_once('x')?;
            Some((w.parse().ok()?, h.parse().ok()?))
        })
        .unwrap_or((800, 600))
}

/// `PREVIEW_FRAME=<n>`: which frame of the preset to render, for a
/// preset whose picture depends on the frame number (an interlaced or
/// flickering CRT, a phosphor afterglow). The real editor takes this
/// from a clock — the picture has to move — so headlessly there has to
/// be a way to name one frame and get it twice. Default 0.
pub fn preview_frame_env() -> usize {
    std::env::var("PREVIEW_FRAME").ok().and_then(|s| s.parse().ok()).unwrap_or(0)
}

pub fn parse_family(arg: Option<&str>, usage: &str) -> Family {
    match arg {
        Some("win98") => Family::Win98,
        Some("xp") => Family::Xp,
        Some("dos") => Family::Dos,
        _ => panic!("{usage}"),
    }
}

/// Handle one verb. `args` is whatever followed it.
pub fn run(verb: &str, args: &mut impl Iterator<Item = String>) -> Option<i32> {
    match verb {
        "--print-args" => {
            let path = args.next().expect("usage: --print-args <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            println!("{}", machine.qemu_args(&player::pc_bios_dir(), None).join(" "));
        }
        "--print-shader-args" => {
            let path = args.next().expect("usage: --print-shader-args <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            println!("{}", player::shader_args(&machine).join(" "));
        }
        "--play" => {
            let path = PathBuf::from(args.next().expect("usage: --play <machine.toml>"));
            let machine = Machine::load(&path).expect("load bundle");
            // The same monitor socket the grid's "Play" opens, so the
            // live half of `--discs`/`--snapshots` can be scripted
            // against a player started this way.
            let dir = path.parent().unwrap_or(Path::new("."));
            let socket = control::socket_path(dir);
            let shelf_path = control::shelf_path(dir);
            machines::publish_shelf(&disc_library::default_path(), &shelf_path);
            let child = player::spawn(&machine, Some(&socket), Some(&shelf_path)).expect("spawn player");
            println!("pid {} qmp {} shelf {}", child.id(), socket.display(), shelf_path.display());
        }
        "--qmp-socket" => {
            // Where a bundle's live-control monitor socket lives, so a
            // script (or a hand-run QEMU standing in for the player) can
            // put one there.
            let path = PathBuf::from(args.next().expect("usage: --qmp-socket <machine.toml>"));
            println!("{}", control::socket_path(path.parent().unwrap_or(Path::new("."))).display());
        }
        "--kvm" => {
            // What the wizard's acceleration hint reads, on its own:
            // this host's answer, not the bundle's setting.
            println!("{}", if player::hw_accel_available() { "available" } else { "not available" });
        }
        "--host-check" => {
            // The other half of `--kvm`: what this host can do for the
            // guest's 3D, asked before a machine is created rather than
            // found as an absence afterwards (ADR-013). The bar is DXVK's
            // own — a Vulkan 1.3 device — and a host below it still runs
            // every guest, through the OpenGL pass-through with WineD3D
            // in the guest. Exits non-zero only when the device is
            // unavailable: a software driver is slow, not absent, and a
            // script asking "can this host do 3D" should hear yes.
            let probe = crate::host_gpu::probe();
            print!("{}", crate::host_gpu::report_text(&probe));
            return Some(if probe.gpu.d3d_available() { 0 } else { 1 });
        }
        "--paths" => {
            // Every companion a launcher would reach for, and where it
            // found it (`paths.rs`). This is what `scripts/package-linux.sh`
            // checks a staged package with — a package whose launcher
            // still answers with the checkout it was built from is not a
            // package — and the first thing to ask of an installed build
            // that says a file is missing.
            match crate::paths::install_prefix() {
                Some(prefix) => println!("prefix       {}", prefix.display()),
                None => println!("prefix       (not installed; a checkout build)"),
            }
            println!("checkout     {}", crate::paths::checkout(".").display());
            println!("player       {}", player::player_binary().display());
            println!("qemu-img     {}", player::qemu_img_binary().display());
            println!("pc-bios      {}", player::pc_bios_dir().display());
            match disc_library::guest_tools_iso() {
                Some(iso) => println!("guest-tools  {}", iso.display()),
                None => println!("guest-tools  (none built or shipped)"),
            }
            match shader_source::presets_dir() {
                Some(dir) => println!("shaders      {}", dir.display()),
                None => println!("shaders      (none; downloadable into {})", shader_source::install_dir().display()),
            }
            println!("machines     {}", library::default_dir().display());
            println!("discs        {}", disc_library::default_path().display());
            println!("profiles     {}", shader_library::default_dir().display());
        }
        "--new" => {
            let usage = "usage: --new <win98|xp|dos> <name> <disk.qcow2>";
            let family = parse_family(args.next().as_deref(), usage);
            let name = args.next().expect(usage);
            let disk = args.next().expect(usage).into();
            let path = library::create(&library::default_dir(), family, name, disk).expect("create bundle");
            println!("{}", path.display());
        }
        "--wizard-new" => {
            // Headless equivalent of the "New machine" window: the real
            // form's `submit`, disk creation via qemu-img included.
            let usage = "usage: --wizard-new <win98|xp|dos> <name> <disk-size-gb>";
            let family = parse_family(args.next().as_deref(), usage);
            let name = args.next().expect(usage);
            let size_gb: u32 = args.next().expect(usage).parse().expect("disk size must be a number");
            let mut form = wizard::Form::with_new_disk(family, name, size_gb);
            match form.submit(&library::default_dir()) {
                Some(path) => println!("{}", path.display()),
                None => panic!("create bundle: {}", form.error.unwrap_or_default()),
            }
        }
        "--wizard-edit" => {
            // Headless equivalent of clicking "Edit…" then "Save": load
            // a bundle, change the fields given, save it back in place.
            // `-` keeps a field as it is.
            let usage =
                "usage: --wizard-edit <machine.toml> <new-name|-> [ram-mb|-] [auto|kvm|tcg|-] [net|nonet] [cpu-speed|-] [boot|-]";
            let path: PathBuf = args.next().expect(usage).into();
            let new_name = args.next().expect(usage);
            let mut form = wizard::Form::default();
            form.open_edit_path(path);
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            if new_name != "-" {
                form.name = new_name;
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(ram) => form.choose_ram_mb(ram.parse().expect("ram must be a number")),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("auto") => form.choose_accel(bundle::Accel::Auto),
                Some("kvm") => form.choose_accel(bundle::Accel::Kvm),
                Some("tcg") => form.choose_accel(bundle::Accel::Tcg),
                Some(other) => panic!("unknown accelerator {other:?}; {usage}"),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("net") => form.choose_network(true),
                Some("nonet") => form.choose_network(false),
                Some(other) => panic!("networking is net or nonet, not {other:?}; {usage}"),
            }
            // The processor and the boot order, by label: these are the
            // two fields the Qt port had no widget for until the form
            // became shared, so they are worth being able to drive.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(label) => form.choose_cpu_speed(
                    bundle::CpuSpeed::ALL
                        .into_iter()
                        .find(|s| s.label().eq_ignore_ascii_case(label))
                        .unwrap_or_else(|| panic!("unknown processor {label:?}; {usage}")),
                ),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(label) => {
                    form.boot = bundle::Boot::ALL
                        .into_iter()
                        .find(|b| b.label().eq_ignore_ascii_case(label))
                        .unwrap_or_else(|| panic!("unknown boot order {label:?}; {usage}"))
                }
            }
            match form.submit(&library::default_dir()) {
                Some(saved) => println!("{}", saved.display()),
                None => panic!("save bundle: {}", form.error.unwrap_or_default()),
            }
        }
        "--optimizations" => {
            // Headless equivalent of the wizard's "Emulation
            // optimizations" section: the real form's checkboxes and its
            // "All defaults" button, then a save. With no changes it
            // just reports, which is also how a bundle is read back
            // after one — the state, and whether it is the shipped one.
            let usage = "usage: --optimizations <machine.toml> [<name> on|off | defaults]...";
            let path: PathBuf = args.next().expect(usage).into();
            let mut form = wizard::Form::default();
            form.open_edit_path(path.clone());
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            let mut changed = false;
            while let Some(arg) = args.next() {
                changed = true;
                if arg == "defaults" {
                    form.reset_optimizations();
                    continue;
                }
                let opt = Optimization::ALL
                    .into_iter()
                    .find(|o| o.key() == arg)
                    .unwrap_or_else(|| panic!("unknown optimization {arg:?}; {usage}"));
                let on = match args.next().as_deref() {
                    Some("on") => true,
                    Some("off") => false,
                    other => panic!("{} takes on or off, not {other:?}; {usage}", opt.key()),
                };
                form.choose_optimization(opt, on);
            }
            if changed && form.submit(&library::default_dir()).is_none() {
                eprintln!("[optimizations] {}", form.error.unwrap_or_default());
                return Some(1);
            }
            // What the section says above the switches — on a machine
            // headed for KVM it says they do nothing, which is the one
            // thing worth seeing from a script too.
            println!("[optimizations] {}", form.optimizations_note());
            for opt in Optimization::ALL {
                println!(
                    "{}\t{}\t{}",
                    opt.key(),
                    if form.optimization_enabled(opt) { "on" } else { "off" },
                    if form.optimizations().is_default(opt) { "default" } else { "changed" }
                );
            }
        }
        "--boot-disc" => {
            // Headless equivalent of a row's "Boot" button: which disc is
            // in the machine's drive when it starts.
            let usage = "usage: --boot-disc <machine.toml> <disc|none>";
            let path: PathBuf = args.next().expect(usage).into();
            let mut shelf = shelf::Shelf::default();
            shelf.open_for_path(path, &disc_library::default_path());
            let disc = args.next().expect(usage);
            shelf.set_boot(if disc == "none" { None } else { Some(disc.into()) });
            match shelf.last_result() {
                Ok(status) => println!("{}", status.unwrap_or("(nothing happened)")),
                Err(e) => {
                    eprintln!("[boot-disc] {e}");
                    return Some(1);
                }
            }
        }
        "--discs" => {
            // Headless equivalent of the shelf window: with no arguments
            // it prints the shared shelf, otherwise it runs the same
            // `add`/`remove` the buttons do. `+tools` stands for the
            // "Add guest-tools ISO" button.
            let library_path = disc_library::default_path();
            machines::import_legacy_discs(&library::scan(&library::default_dir()), &library_path);
            let mut shelf = shelf::Shelf::default();
            shelf.open_library(&library_path);
            let mut removing = false;
            for arg in args.by_ref() {
                match arg.as_str() {
                    "add" => removing = false,
                    "remove" => removing = true,
                    "+tools" => shelf.add_guest_tools(),
                    other if removing => shelf.remove(Path::new(other)),
                    other => shelf.add(other.into()),
                }
            }
            // The egui window saves at the end of the frame it was
            // edited in; headlessly there is no frame, so flush here.
            shelf.flush().expect("save the shelf");
            if let Err(e) = shelf.last_result() {
                eprintln!("[discs] {e}");
            }
            for disc in shelf.discs() {
                println!("{}\t{}", disc.label, disc.path.display());
            }
        }
        "--insert-disc" => {
            // Headless equivalent of the shelf's live "Insert"/"Eject"
            // buttons, against the machine's running monitor.
            let usage = "usage: --insert-disc <machine.toml> <disc|eject>";
            let path: PathBuf = args.next().expect(usage).into();
            let what = args.next().expect(usage);
            let mut shelf = shelf::Shelf::default();
            shelf.open_for_path(path, &disc_library::default_path());
            if what == "eject" {
                shelf.eject_live();
            } else {
                shelf.insert_live(Path::new(&what));
            }
            match shelf.last_result() {
                Ok(status) => println!("{}", status.unwrap_or("(nothing happened)")),
                Err(e) => {
                    eprintln!("[disc-shelf] {e}");
                    return Some(1);
                }
            }
        }
        "--snapshots" => {
            // Headless equivalent of the "Snapshots…" window: the same
            // operations its buttons run, over a bundle's disk.
            let usage = "usage: --snapshots [--live] <machine.toml> [take|delete|restore <name>]";
            let mut next = args.next();
            // `--live` drives a *running* machine's monitor instead of
            // qemu-img, the way the window does when its player is up.
            let live = next.as_deref() == Some("--live");
            if live {
                next = args.next();
            }
            let path: PathBuf = next.expect(usage).into();
            let mut window = snaps::Snapshots::default();
            window.open_for_path(&path, live);
            match (args.next().as_deref(), args.next()) {
                (Some("take"), Some(name)) => window.take(&name),
                (Some("delete"), Some(name)) => window.drop_snapshot(&name),
                (Some("restore"), Some(name)) => window.revert(&name),
                (None, _) => {}
                _ => panic!("{usage}"),
            }
            // A live operation is a QMP *job*: it returns as soon as the
            // job exists and finishes later, so wait for it here the way
            // the window's repaint tick or timer does.
            window.wait_for_job(Duration::from_secs(120));
            print_snapshots(&window);
        }
        "--shaders" => {
            // What the profile manager's preset row reads: where this
            // machine's `.slangp` collection is, or nothing — which is
            // what puts the "Download presets" button on screen, and
            // what "Browse…" opens on when the field is empty.
            match shader_source::presets_dir() {
                Some(dir) => println!("{}", dir.display()),
                None => println!("(none; would install into {})", shader_source::install_dir().display()),
            }
        }
        "--download-shaders" => {
            // The button's own work, without a window: fetch and unpack
            // the collection, printing what arrived.
            let dest = args.next().map(PathBuf::from).unwrap_or_else(shader_source::install_dir);
            let bytes = std::sync::atomic::AtomicU64::new(0);
            match shader_source::fetch(&dest, &bytes) {
                Ok(()) => println!(
                    "{:.1} MB -> {} ({})",
                    bytes.load(std::sync::atomic::Ordering::Relaxed) as f64 / 1_000_000.0,
                    dest.display(),
                    if shader_source::has_presets(&dest) { "presets found" } else { "NO PRESETS" }
                ),
                Err(e) => {
                    eprintln!("[shaders] {e}");
                    return Some(1);
                }
            }
        }
        "--browse-start" => {
            // Where a path field's "Browse…" would open: the value's own
            // directory, or — for an empty preset field — the preset
            // collection. The dialog itself is modal and needs a human,
            // so this checks the decision, not the dialog.
            let value = args.next().unwrap_or_default();
            match browse::browse_start(&value, shader_source::presets_dir().as_deref()) {
                Some(dir) => println!("{}", dir.display()),
                None => println!("(OS default)"),
            }
        }
        "--new-shader-profile" => {
            let usage = "usage: --new-shader-profile <name> <preset.slangp>";
            let name = args.next().expect(usage);
            let preset = args.next().expect(usage).into();
            let path = shader_library::create(&shader_library::default_dir(), name, preset).expect("create profile");
            println!("{}", path.display());
        }
        "--set-shader-param" => {
            let usage = "usage: --set-shader-param <profile.toml> <param> <value>";
            let path: PathBuf = args.next().expect(usage).into();
            let param = args.next().expect(usage);
            let value: f32 = args.next().expect(usage).parse().expect("value must be a number");
            let mut profile = shader_profile::ShaderProfile::load(&path).expect("load profile");
            profile.params.insert(param, value);
            profile.save(&path).expect("save profile");
            println!("{}", profile.params_arg().unwrap_or_default());
        }
        "--list-shader-params" => {
            let preset = args.next().expect("usage: --list-shader-params <preset.slangp>");
            let params = shader_profile::parameter_meta(Path::new(&preset)).expect("parse preset");
            for p in params {
                println!("{} [{}..{}] step {} = {} — {}", p.id, p.minimum, p.maximum, p.step, p.default, p.description);
            }
        }
        "--assign-shader" => {
            let usage = "usage: --assign-shader <machine.toml> <profile-id-or-(none)>";
            let path: PathBuf = args.next().expect(usage).into();
            let id = args.next().expect(usage);
            let mut machine = Machine::load(&path).expect("load bundle");
            machine.shader_profile = if id == "(none)" { None } else { Some(id) };
            machine.save(&path).expect("save bundle");
        }
        "--preview-shader" => {
            // Headless equivalent of the profile editor's live preview
            // pane: the real `preview::Preview`, on a real (if
            // windowless) adapter and device, proving the image-decode
            // and render path without a GUI click. Both binaries answer
            // this identically because it *is* the same code now — the
            // byte-identical PNGs doc 07 checks the two builds against.
            let usage = "usage: --preview-shader <preset.slangp> <image> <out.png> [name=value,...]";
            let preset: PathBuf = args.next().expect(usage).into();
            let image: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let params = shader_profile::parse_params(&args.next().unwrap_or_default());
            let (w, h) = preview_area_env();
            let mut preview = preview::Preview::headless().expect("a headless wgpu device");
            // The editor's preview follows a clock, so that a preset
            // whose picture depends on the frame number actually moves in
            // it; here one frame is named instead, so the same command
            // twice is the same PNG twice.
            preview.pin_frame(preview_frame_env());
            preview.update(&preset, &params, &image, w, h);
            if let Some(err) = preview.error() {
                eprintln!("[preview] {err}");
            }
            // Whether the editor would be redrawing this preset at all,
            // which is as much a part of what the preview does as the
            // pixels are — and the only way a test can see the decision.
            println!("{}", if preview.frame_interval().is_some() { "animated" } else { "still" });
            preview.dump_png(&out).expect("no frame rendered");
        }
        _ => return None,
    }
    Some(0)
}

/// The snapshot window's own output, for the verbs that drive it.
pub fn print_snapshots(window: &snaps::Snapshots) {
    if let Some(status) = window.status() {
        println!("[snapshots] {status}");
    }
    if let Some(err) = window.error() {
        eprintln!("[snapshots] {err}");
    }
    for snap in window.snapshots() {
        println!("{}\t{}\t{}\t{}", snap.id, snap.name, snap.date_label(), snap.size_label());
    }
}
