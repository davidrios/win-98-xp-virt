//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: the library grid (`library.rs`) over the
//! `machine.toml` bundle format (`bundle.rs`); no guided creation,
//! spawning a player, or thumbnails yet.

mod bundle;
mod library;

struct LauncherApp {
    library_dir: std::path::PathBuf,
    entries: Vec<library::LibraryEntry>,
}

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
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
                    ui.strong("Location");
                    ui.end_row();
                    for entry in &self.entries {
                        ui.label(&entry.machine.name);
                        ui.label(match entry.machine.family {
                            bundle::Family::Win98 => "Win98",
                            bundle::Family::Xp => "XP",
                        });
                        ui.label(entry.dir.display().to_string());
                        ui.end_row();
                    }
                });
            }
            ui.add_space(8.0);
            ui.add_enabled(false, egui::Button::new("New machine…"));
        });
    }
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
            let pc_bios = std::path::Path::new("qemu/pc-bios");
            println!("{}", machine.qemu_args(pc_bios).join(" "));
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
        _ => {}
    }

    let library_dir = library::default_dir();
    let entries = library::scan(&library_dir);
    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
        Box::new(|_cc| Ok(Box::new(LauncherApp { library_dir, entries }))),
    )
}
