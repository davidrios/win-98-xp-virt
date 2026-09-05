//! The guided creation wizard (doc 07): family -> name -> disk size ->
//! install media -> a bundle written from doc 06's reference defaults.
//! An advanced toggle edits the raw TOML directly instead — still never
//! a QEMU command line, per doc 07. The same form doubles as the "Edit
//! machine" dialog for an existing bundle (`open_edit`); `submit` writes
//! back in place instead of reserving a new library directory.

use crate::bundle::{Family, Machine};
use crate::filepicker;
use crate::library;
use crate::player;
use std::path::{Path, PathBuf};

const DISK_FILTER: filepicker::Filter = ("Disk images", &["qcow2", "img", "raw"]);
const DISC_FILTER: filepicker::Filter = ("Disc images", &["iso", "cue", "ccd", "mds"]);

/// What editing an existing bundle needs to preserve: fields this form
/// doesn't expose (RAM, the shader override) and any disc-shelf entries
/// beyond the first (the form only edits the "install media" slot), so a
/// quick edit can't silently discard them. `original_toml` is the file's
/// exact current text, used as the advanced box's starting point instead
/// of a reconstruction — no information loss even for a field this form
/// (or a future one) doesn't model.
struct EditTarget {
    bundle_path: PathBuf,
    ram_mb: u32,
    shader: Option<PathBuf>,
    extra_discs: Vec<PathBuf>,
    original_toml: String,
}

pub struct Wizard {
    pub open: bool,
    family: Family,
    name: String,
    existing_disk: bool,
    disk_path: String,
    disk_size_gb: u32,
    install_media: String,
    /// A shader profile id (`shader_library`), or `None` for the app
    /// default. Doesn't touch `EditTarget::shader` — the two overrides
    /// are independent (see `bundle::Machine`).
    shader_profile: Option<String>,
    advanced: bool,
    advanced_toml: String,
    error: Option<String>,
    editing: Option<EditTarget>,
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
            shader_profile: None,
            advanced: false,
            advanced_toml: String::new(),
            error: None,
            editing: None,
        }
    }
}

impl Wizard {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Wizard { open: true, ..Default::default() };
    }

    /// Open the form pre-filled from an existing bundle, to edit it in
    /// place instead of creating a new one.
    pub fn open_edit(&mut self, machine: &Machine, bundle_path: PathBuf) {
        let original_toml = std::fs::read_to_string(&bundle_path).unwrap_or_default();
        *self = Wizard {
            open: true,
            family: machine.family,
            name: machine.name.clone(),
            existing_disk: true,
            disk_path: machine.disk.display().to_string(),
            install_media: machine.discs.first().map(|d| d.display().to_string()).unwrap_or_default(),
            shader_profile: machine.shader_profile.clone(),
            editing: Some(EditTarget {
                bundle_path,
                ram_mb: machine.ram_mb,
                shader: machine.shader.clone(),
                extra_discs: machine.discs.iter().skip(1).cloned().collect(),
                original_toml,
            }),
            ..Default::default()
        };
    }

    /// Headless field access for scripted testing (`main.rs`'s
    /// `--wizard-edit`), so an edit's actual field change can be driven
    /// without a GUI click.
    pub fn set_name(&mut self, name: String) {
        self.name = name;
    }

    /// Headless construction (a debug verb; see `main.rs`'s `--wizard-new`)
    /// with the same fields the window's widgets would otherwise have set,
    /// so the wizard's actual disk-creation/save logic (`submit`, below)
    /// can be exercised without clicking through the GUI.
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Wizard {
        Wizard { family, name, disk_size_gb, ..Default::default() }
    }

    /// Renders the wizard window if open. `shader_profiles` is the
    /// current shader-profile library (`shader_library::scan`), for the
    /// "Shader profile" picker. Returns the bundle's path once a machine
    /// has actually been created or saved, so the caller can rescan the
    /// library.
    pub fn show(
        &mut self,
        ctx: &egui::Context,
        library_dir: &Path,
        shader_profiles: &[crate::shader_library::ProfileEntry],
    ) -> Option<PathBuf> {
        if !self.open {
            return None;
        }
        let editing = self.editing.is_some();
        let mut done = None;
        let mut still_open = true;
        egui::Window::new(if editing { "Edit machine" } else { "New machine" })
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
                if editing {
                    filepicker::path_field(ui, "Disk path", &mut self.disk_path, Some(DISK_FILTER));
                } else {
                    ui.checkbox(&mut self.existing_disk, "Use an existing disk image");
                    if self.existing_disk {
                        filepicker::path_field(ui, "Disk path", &mut self.disk_path, Some(DISK_FILTER));
                    } else {
                        ui.horizontal(|ui| {
                            ui.label("New disk size (GB)");
                            ui.add(egui::DragValue::new(&mut self.disk_size_gb).range(1..=128));
                        });
                    }
                }
                filepicker::path_field(ui, "Install media (optional)", &mut self.install_media, Some(DISC_FILTER));
                ui.separator();
                egui::ComboBox::from_label("Shader profile")
                    .selected_text(
                        self.shader_profile
                            .as_deref()
                            .and_then(|id| shader_profiles.iter().find(|e| crate::shader_library::id_of(&e.path) == id))
                            .map(|e| e.profile.name.as_str())
                            .unwrap_or("(default)"),
                    )
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.shader_profile, None, "(default)");
                        for entry in shader_profiles {
                            let id = crate::shader_library::id_of(&entry.path);
                            ui.selectable_value(&mut self.shader_profile, Some(id), &entry.profile.name);
                        }
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
                if ui.button(if editing { "Save" } else { "Create" }).clicked() {
                    match self.submit(library_dir) {
                        Ok(path) => {
                            done = Some(path);
                            self.error = None;
                        }
                        Err(e) => self.error = Some(e.to_string()),
                    }
                }
            });
        self.open = still_open && done.is_none();
        done
    }

    /// The `Machine` the current field values describe, given the disk
    /// path to use (a fresh disk's path isn't known until it's created,
    /// so callers that might still need to do that pass it in rather
    /// than this reading `disk_path` itself). Shared by the advanced
    /// box's default and the non-advanced submit path so they can't
    /// silently disagree.
    fn build_machine(&self, disk: PathBuf) -> Machine {
        let mut machine = match &self.editing {
            Some(edit) => Machine {
                name: self.name.clone(),
                family: self.family,
                ram_mb: edit.ram_mb,
                disk,
                discs: Vec::new(),
                shader_profile: None,
                shader: edit.shader.clone(),
            },
            None => Machine::reference(self.family, self.name.clone(), disk),
        };
        machine.shader_profile = self.shader_profile.clone();
        if !self.install_media.trim().is_empty() {
            machine.discs.push(self.install_media.clone().into());
        }
        if let Some(edit) = &self.editing {
            machine.discs.extend(edit.extra_discs.iter().cloned());
        }
        machine
    }

    fn preview_toml(&self) -> String {
        if let Some(edit) = &self.editing {
            return edit.original_toml.clone();
        }
        let disk: PathBuf = if self.existing_disk { self.disk_path.clone().into() } else { "disk.qcow2".into() };
        toml::to_string_pretty(&self.build_machine(disk)).unwrap_or_default()
    }

    pub fn submit(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
        if self.name.trim().is_empty() {
            return Err(std::io::Error::other("a name is required"));
        }
        if let Some(edit) = &self.editing {
            let bundle_path = edit.bundle_path.clone();
            if self.advanced {
                // Validate before writing: a bad hand-edit shouldn't
                // silently corrupt the library with an unreadable bundle.
                toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
                std::fs::write(&bundle_path, &self.advanced_toml)?;
                return Ok(bundle_path);
            }
            let disk = PathBuf::from(&self.disk_path);
            self.build_machine(disk).save(&bundle_path)?;
            return Ok(bundle_path);
        }
        let dir = library::reserve_dir(library_dir, &self.name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        if self.advanced {
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
        self.build_machine(disk_path).save(&bundle_path)?;
        Ok(bundle_path)
    }
}
