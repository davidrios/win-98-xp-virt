//! The guided creation window, in egui: widgets over
//! `launcher_core::wizard::Form`, which is the whole form — its fields,
//! the defaults that follow the family until someone chooses otherwise,
//! the per-family memory clamp, what it writes, and the sentences under
//! each row. Nothing here decides anything; it draws what the form says
//! and calls back in when something is clicked.
//!
//! That split is what makes the "…_chosen" rule (memory, the
//! accelerator, the processor and the NIC follow the family until
//! someone touches them) impossible to get wrong in a new widget: the
//! only way to set one of those fields is `choose_*`, which applies the
//! rule.

use crate::filepicker;
use launcher_core::bundle::{Accel, Boot, CpuSpeed, Family, Optimization};
use launcher_core::shader_library;
use launcher_core::wizard::{Form, DISK_FILTER, FLOPPY_FILTER, MEDIA_FILTER};
use std::path::{Path, PathBuf};

/// Renders the wizard window if open. `shader_profiles` is the current
/// shader-profile library, for the "Shader profile" picker. Returns the
/// bundle's path once a machine has been created or saved, so the caller
/// can rescan the library.
pub fn show(
    form: &mut Form,
    ctx: &egui::Context,
    library_dir: &Path,
    shader_profiles: &[shader_library::ProfileEntry],
) -> Option<PathBuf> {
    if !form.open {
        return None;
    }
    let editing = form.is_editing();
    let mut done = None;
    let mut still_open = true;
    egui::Window::new(form.title())
        .open(&mut still_open)
        .collapsible(false)
        .resizable(false)
        .show(ctx, |ui| {
            // The fields scroll, the buttons don't. This window already
            // reached close to the bottom of a small screen, and opening
            // the optimizations section is enough to push "Save" off it
            // — a form whose save button cannot be reached is worse than
            // one that scrolls. The height comes from the screen rather
            // than a constant, so the scroll bar appears only when the
            // window really would not fit.
            let body_height = (ctx.viewport_rect().height() - 140.0).max(240.0);
            egui::ScrollArea::vertical()
                .max_height(body_height)
                .auto_shrink([false, true])
                .show(ui, |ui| fields_ui(ui, form, shader_profiles, editing));
            ui.separator();
            if let Some(err) = &form.error {
                ui.colored_label(egui::Color32::RED, err);
            }
            if ui.button(if editing { "Save" } else { "Create" }).clicked() {
                done = form.submit(library_dir);
            }
        });
    // `submit` closes the form itself; the title-bar X is this.
    form.open = still_open && done.is_none();
    done
}

/// Every field, in one function so the scroll area around them is one
/// line at the call site rather than an extra level of indentation over
/// the whole form.
fn fields_ui(
    ui: &mut egui::Ui,
    form: &mut Form,
    shader_profiles: &[shader_library::ProfileEntry],
    editing: bool,
) {
    // egui reads a widget's new value and compares it with the
    // old one in the same frame, which is the immediate-mode way
    // of noticing a change; the form's `choose_*` then applies
    // what follows from it.
    let mut family = form.family();
    egui::ComboBox::from_label("Family")
        .selected_text(family.label())
        .show_ui(ui, |ui| {
            for f in Family::ALL {
                ui.selectable_value(&mut family, f, f.label());
            }
        });
    if family != form.family() {
        form.choose_family(family);
    }
    ui.horizontal(|ui| {
        ui.label("Name");
        ui.text_edit_singleline(&mut form.name);
    });
    ui.separator();
    memory_ui(ui, form);
    cpu_speed_ui(ui, form);
    accel_ui(ui, form);
    graphics_ui(ui, form);
    network_ui(ui, form);
    optimizations_ui(ui, form);
    ui.separator();
    if editing {
        filepicker::path_field(ui, "Disk path", &mut form.disk_path, Some(DISK_FILTER));
    } else {
        ui.checkbox(&mut form.existing_disk, "Use an existing disk image");
        if form.existing_disk {
            filepicker::path_field(ui, "Disk path", &mut form.disk_path, Some(DISK_FILTER));
        } else {
            ui.horizontal(|ui| {
                ui.label("New disk size (GB)");
                ui.add(egui::DragValue::new(&mut form.disk_size_gb).range(1..=128));
            });
        }
    }
    filepicker::path_field(ui, "Install media (optional)", &mut form.install_media, Some(MEDIA_FILTER));
    filepicker::path_field(ui, "Floppy (optional)", &mut form.floppy, Some(FLOPPY_FILTER));
    egui::ComboBox::from_label("Boot from")
        .selected_text(form.boot.label())
        .show_ui(ui, |ui| {
            for b in Boot::ALL {
                ui.selectable_value(&mut form.boot, b, b.label());
            }
        });
    if let Some(note) = form.boot_note() {
        ui.small(note);
    }
    ui.separator();
    shader_profile_ui(ui, form, shader_profiles);
    ui.separator();
    ui.checkbox(&mut form.advanced, "Advanced: edit machine.toml directly");
    if form.advanced {
        form.fill_advanced();
        ui.add(egui::TextEdit::multiline(&mut form.advanced_toml).code_editor().desired_rows(10));
    }
}

/// The RAM row. The range is per family, so the form cannot produce a
/// Win98 machine with more memory than Win98 can boot with; a clamped
/// value is corrected in place rather than refused at save time, and the
/// reason is on screen next to it.
fn memory_ui(ui: &mut egui::Ui, form: &mut Form) {
    let range = form.ram_range();
    let mut ram_mb = form.ram_mb();
    ui.horizontal(|ui| {
        ui.label("Memory (MB)");
        if ui.add(egui::DragValue::new(&mut ram_mb).speed(16.0).range(range)).changed() {
            form.choose_ram_mb(ram_mb);
        }
        if ui.add_enabled(!form.ram_is_default(), egui::Button::new("Default")).clicked() {
            form.reset_ram();
        }
    });
    if let Some(note) = form.ram_note() {
        ui.small(note);
    }
}

/// The processor the guest should feel like. Named machines rather than
/// a number, because "how many instructions per second" is not a thing
/// anyone knows about their DOS game, while "it wants a 486" is written
/// on the box.
fn cpu_speed_ui(ui: &mut egui::Ui, form: &mut Form) {
    let mut cpu_speed = form.cpu_speed();
    ui.horizontal(|ui| {
        egui::ComboBox::from_label("Processor")
            .selected_text(cpu_speed.label())
            .show_ui(ui, |ui| {
                for s in CpuSpeed::ALL {
                    ui.selectable_value(&mut cpu_speed, s, s.label());
                }
            });
        if cpu_speed != form.cpu_speed() {
            form.choose_cpu_speed(cpu_speed);
        }
        if ui.add_enabled(!form.cpu_speed_is_default(), egui::Button::new("Default")).clicked() {
            form.reset_cpu_speed();
        }
    });
    for note in form.cpu_speed_notes() {
        ui.small(*note);
    }
}

/// The acceleration row, plus what this host can actually do — the
/// picker alone would leave "Automatic" meaning something invisible.
fn accel_ui(ui: &mut egui::Ui, form: &mut Form) {
    let mut accel = form.accel();
    ui.horizontal(|ui| {
        egui::ComboBox::from_label("Acceleration")
            .selected_text(accel.label())
            .show_ui(ui, |ui| {
                for a in Accel::ALL {
                    ui.selectable_value(&mut accel, a, a.label());
                }
            });
        if accel != form.accel() {
            form.choose_accel(accel);
        }
        if ui.add_enabled(!form.accel_is_default(), egui::Button::new("Default")).clicked() {
            form.reset_accel();
        }
    });
    let note = form.accel_note();
    if note.warning {
        ui.colored_label(egui::Color32::from_rgb(200, 140, 0), note.text);
    } else {
        // The form composes a second line when there is one (Win98 under
        // KVM); egui's `small` takes the newline as-is.
        ui.small(note.text);
    }
}

/// No row of its own: what this host will give the guest's 3D, which the
/// form decides and words (`graphics_notes`, ADR-013). Orange only for
/// the software-Vulkan case, which is the one that will disappoint.
fn graphics_ui(ui: &mut egui::Ui, form: &Form) {
    let Some(note) = form.graphics_note() else { return };
    if note.warning {
        ui.colored_label(egui::Color32::from_rgb(200, 140, 0), note.text);
    } else {
        ui.small(note.text);
    }
}

/// The networking row: one checkbox, because there is one question here
/// — does this machine have a network card. What follows is the form's
/// to say (`network_notes`), since "networking" otherwise sounds like
/// the guest is being put on the LAN.
fn network_ui(ui: &mut egui::Ui, form: &mut Form) {
    let mut network = form.network();
    if ui.checkbox(&mut network, "Networking").changed() {
        form.choose_network(network);
    }
    for note in form.network_notes() {
        ui.small(*note);
    }
}

/// Our own emulator fast paths, one checkbox each, behind a disclosure
/// because seven switches nobody needs to touch would push the fields
/// that matter off the bottom of the window. The header carries the
/// count, so a machine with one turned off says so while closed.
fn optimizations_ui(ui: &mut egui::Ui, form: &mut Form) {
    egui::CollapsingHeader::new(format!("Emulation optimizations — {}", form.optimizations_summary()))
        .id_salt("optimizations")
        .show(ui, |ui| {
            ui.small(form.optimizations_note());
            for opt in Optimization::ALL {
                let mut on = form.optimization_enabled(opt);
                if ui.checkbox(&mut on, opt.label()).changed() {
                    form.choose_optimization(opt, on);
                }
                ui.small(opt.note());
            }
            if ui.add_enabled(!form.optimizations_are_default(), egui::Button::new("All defaults")).clicked() {
                form.reset_optimizations();
            }
        });
}

fn shader_profile_ui(ui: &mut egui::Ui, form: &mut Form, profiles: &[shader_library::ProfileEntry]) {
    egui::ComboBox::from_label("Shader profile")
        .selected_text(
            form.shader_profile
                .as_deref()
                .and_then(|id| profiles.iter().find(|e| shader_library::id_of(&e.path) == id))
                .map(|e| e.profile.name.as_str())
                .unwrap_or("(default)"),
        )
        .show_ui(ui, |ui| {
            ui.selectable_value(&mut form.shader_profile, None, "(default)");
            for entry in profiles {
                let id = shader_library::id_of(&entry.path);
                ui.selectable_value(&mut form.shader_profile, Some(id), &entry.profile.name);
            }
        });
}
