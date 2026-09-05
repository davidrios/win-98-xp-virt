//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: a window with the (empty) library grid, and the
//! `machine.toml` bundle format (`bundle.rs`); no library scanning,
//! thumbnails or guided creation yet.

mod bundle;

struct LauncherApp;

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading("win98-xp-virt");
            ui.add_space(8.0);
            ui.label("No machines yet.");
            ui.add_enabled(false, egui::Button::new("New machine…"));
        });
    }
}

fn main() -> eframe::Result {
    // Debug/advanced-drawer aids (doc 07), until the wizard and library
    // grid exist: `--new` bootstraps a bundle from the doc 06 reference
    // defaults, `--print-args` shows the qemu-system-i386 command line it
    // translates to. Neither opens a window.
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("--print-args") => {
            let path = args.next().expect("usage: launcher --print-args <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            let pc_bios = std::path::Path::new("qemu/pc-bios");
            println!("{}", machine.qemu_args(pc_bios).join(" "));
            return Ok(());
        }
        Some("--new") => {
            let usage = "usage: launcher --new <win98|xp> <name> <disk.qcow2> <out.toml>";
            let family = match args.next().as_deref() {
                Some("win98") => bundle::Family::Win98,
                Some("xp") => bundle::Family::Xp,
                _ => panic!("{usage}"),
            };
            let name = args.next().expect(usage);
            let disk = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let machine = bundle::Machine::reference(family, name, disk);
            machine.save(std::path::Path::new(&out)).expect("save bundle");
            return Ok(());
        }
        _ => {}
    }

    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
        Box::new(|_cc| Ok(Box::new(LauncherApp))),
    )
}
