//! The guided creation wizard (doc 07): family -> name -> disk size ->
//! install media -> a bundle written from doc 06's reference defaults.
//! An advanced toggle edits the raw TOML directly instead — still never
//! a QEMU command line, per doc 07.

use crate::bundle::{Family, Machine};
use crate::library;
use crate::player;
use std::path::{Path, PathBuf};

pub struct Wizard {
    pub open: bool,
    family: Family,
    name: String,
    existing_disk: bool,
    disk_path: String,
    disk_size_gb: u32,
    install_media: String,
    advanced: bool,
    advanced_toml: String,
    error: Option<String>,
}

impl Default for Wizard {
    fn default() -> Self {
        Wizard {
            open: false,
            family: Family::Win98,
            name: String::new(),
            existing_disk: false,
            disk_path: String::new(),
            disk_size_gb: 2,
            install_media: String::new(),
            advanced: false,
            advanced_toml: String::new(),
            error: None,
        }
    }
}

impl Wizard {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Wizard { open: true, ..Default::default() };
    }

    /// Headless construction (a debug verb; see `main.rs`'s `--wizard-new`)
    /// with the same fields the window's widgets would otherwise have set,
    /// so the wizard's actual disk-creation/save logic (`create`, below)
    /// can be exercised without clicking through the GUI.
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Wizard {
        Wizard { family, name, disk_size_gb, ..Default::default() }
    }

    /// Renders the wizard window if open. Returns the new bundle's path
    /// once a machine has actually been created, so the caller can
    /// rescan the library.
    pub fn show(&mut self, ctx: &egui::Context, library_dir: &Path) -> Option<PathBuf> {
        if !self.open {
            return None;
        }
        let mut created = None;
        let mut still_open = true;
        egui::Window::new("New machine")
            .open(&mut still_open)
            .collapsible(false)
            .resizable(false)
            .show(ctx, |ui| {
                egui::ComboBox::from_label("Family")
                    .selected_text(match self.family {
                        Family::Win98 => "Win98",
                        Family::Xp => "XP",
                    })
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.family, Family::Win98, "Win98");
                        ui.selectable_value(&mut self.family, Family::Xp, "XP");
                    });
                ui.horizontal(|ui| {
                    ui.label("Name");
                    ui.text_edit_singleline(&mut self.name);
                });
                ui.separator();
                ui.checkbox(&mut self.existing_disk, "Use an existing disk image");
                if self.existing_disk {
                    ui.horizontal(|ui| {
                        ui.label("Disk path");
                        ui.text_edit_singleline(&mut self.disk_path);
                    });
                } else {
                    ui.horizontal(|ui| {
                        ui.label("New disk size (GB)");
                        ui.add(egui::DragValue::new(&mut self.disk_size_gb).range(1..=128));
                    });
                }
                ui.horizontal(|ui| {
                    ui.label("Install media (optional)");
                    ui.text_edit_singleline(&mut self.install_media);
                });
                ui.separator();
                ui.checkbox(&mut self.advanced, "Advanced: edit machine.toml directly");
                if self.advanced {
                    if self.advanced_toml.is_empty() {
                        self.advanced_toml = self.preview_toml();
                    }
                    ui.add(
                        egui::TextEdit::multiline(&mut self.advanced_toml)
                            .code_editor()
                            .desired_rows(10),
                    );
                }
                if let Some(err) = &self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
                if ui.button("Create").clicked() {
                    match self.create(library_dir) {
                        Ok(path) => {
                            created = Some(path);
                            self.error = None;
                        }
                        Err(e) => self.error = Some(e.to_string()),
                    }
                }
            });
        self.open = still_open && created.is_none();
        created
    }

    fn preview_toml(&self) -> String {
        let disk: PathBuf = if self.existing_disk { self.disk_path.clone().into() } else { "disk.qcow2".into() };
        let mut machine = Machine::reference(self.family, self.name.clone(), disk);
        if !self.install_media.is_empty() {
            machine.discs.push(self.install_media.clone().into());
        }
        toml::to_string_pretty(&machine).unwrap_or_default()
    }

    pub fn create(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
        if self.name.trim().is_empty() {
            return Err(std::io::Error::other("a name is required"));
        }
        let dir = library::reserve_dir(library_dir, &self.name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        if self.advanced {
            // Validate before writing: a bad hand-edit shouldn't silently
            // corrupt the library with an unreadable bundle.
            toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
            std::fs::write(&bundle_path, &self.advanced_toml)?;
            return Ok(bundle_path);
        }
        let disk_path = if self.existing_disk {
            PathBuf::from(&self.disk_path)
        } else {
            let disk_path = dir.join("disk.qcow2");
            player::create_disk(&disk_path, self.disk_size_gb)?;
            disk_path
        };
        let mut machine = Machine::reference(self.family, self.name.clone(), disk_path);
        if !self.install_media.is_empty() {
            machine.discs.push(self.install_media.clone().into());
        }
        machine.save(&bundle_path)?;
        Ok(bundle_path)
    }
}
