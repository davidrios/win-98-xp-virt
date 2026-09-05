//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: a window with the (empty) library grid; no machine
//! bundle format, thumbnails or guided creation yet.

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
    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
        Box::new(|_cc| Ok(Box::new(LauncherApp))),
    )
}
