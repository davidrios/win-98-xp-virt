//! A C ABI over `launcher-core`, so a front end that is not Rust can
//! drive the same models the egui and Qt builds do — a native macOS app
//! in Swift is the case this was shaped for (Swift imports a C header
//! directly, with no bridge crate), but anything that speaks C works.
//!
//! **This adds no behaviour.** Every function here is a thin wrapper
//! over a `launcher-core` model, so a third front end gets the same
//! machine library, the same wizard rules, the same disc shelf, the same
//! snapshot state machine and the same shader profile editor the other
//! two have — including the parts that are easy to get subtly wrong
//! (which defaults follow the family, when `qemu-img` is safe to use,
//! keeping only the parameters the user actually overrode). Writing the
//! views is the work; none of this is.
//!
//! ## The shape
//!
//! Each window is an **opaque handle** created by `lc_*_new` and
//! released by `lc_*_free`. Rows are addressed by index and read one
//! field at a time, because that is what crosses a C boundary cleanly —
//! and it is also how the Qt build's `QAbstractListModel::data` already
//! reads them, so nothing was bent to fit.
//!
//! ## The rules a caller must follow
//!
//! * **Strings out are owned by the caller**: every `char *` returned
//!   here was allocated by Rust and must be handed back to
//!   `lc_string_free`. A getter never returns `NULL` for "empty" — it
//!   returns `""` — so `NULL` means only "no such row".
//! * **Strings in are borrowed**, must be UTF-8, and are copied before
//!   the call returns.
//! * **A handle is not thread-safe** and must be used from one thread at
//!   a time. That is not a limitation in practice: these models front a
//!   GUI, and the two Rust front ends drive them from their UI thread.
//! * **Nothing here blocks on a guest.** The long operations are the
//!   ones that already have a poll: a live snapshot is a QMP job
//!   (`lc_snapshots_poll` while `lc_snapshots_job_pending`), and the
//!   preset download runs on its own thread (`lc_editor_preset_state`).
//!
//! `include/launcher_core.h` is the header, hand-written and kept beside
//! this file; `examples/smoke.c` is a program that exercises it, built
//! and run by `scripts/test.sh host` so the ABI cannot rot.

use launcher_core::bundle::{Accel, Boot, CpuSpeed, Family};
use launcher_core::{editor, machines, preview, shelf, snaps, wizard};
use std::ffi::{c_char, CStr, CString};
use std::path::{Path, PathBuf};

// --- strings ---------------------------------------------------------

/// Hand a Rust-allocated string to the caller. Always non-NULL: an empty
/// value is `""`, so `NULL` can mean "no such row" and nothing else.
fn out(s: impl Into<Vec<u8>>) -> *mut c_char {
    // A NUL inside would be a bug in a path or label we produced; there
    // is nothing useful to return but an empty string.
    CString::new(s).unwrap_or_default().into_raw()
}

fn out_opt(s: Option<&str>) -> *mut c_char {
    out(s.unwrap_or_default())
}

/// Borrow a caller's string. Empty for NULL or invalid UTF-8, which is
/// the same thing an empty field means everywhere in these models.
///
/// # Safety
/// `s` must be NULL or a NUL-terminated string valid for this call.
unsafe fn borrow<'a>(s: *const c_char) -> &'a str {
    if s.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(s) }.to_str().unwrap_or("")
}

/// # Safety
/// `s` must be NULL or a pointer returned by a function in this library
/// and not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// Turn a handle pointer into a reference, or return `$ret` if it is
/// NULL. Every entry point starts with one of these, so a caller that
/// passes NULL gets a defined answer rather than a crash.
macro_rules! handle {
    ($p:ident, $ret:expr) => {
        match unsafe { $p.as_ref() } {
            Some(h) => h,
            None => return $ret,
        }
    };
}

macro_rules! handle_mut {
    ($p:ident, $ret:expr) => {
        match unsafe { $p.as_mut() } {
            Some(h) => h,
            None => return $ret,
        }
    };
}

// --- the machine library ---------------------------------------------

pub struct LcMachines(machines::Machines);

/// A model on the default directories, already scanned.
#[no_mangle]
pub extern "C" fn lc_machines_new() -> *mut LcMachines {
    Box::into_raw(Box::new(LcMachines(machines::Machines::load())))
}

/// # Safety
/// `m` must be NULL or a handle from `lc_machines_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_free(m: *mut LcMachines) {
    if !m.is_null() {
        drop(unsafe { Box::from_raw(m) });
    }
}

/// # Safety
/// `m` must be a live handle from `lc_machines_new`.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_refresh(m: *mut LcMachines) {
    handle_mut!(m, ()).0.refresh();
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_refresh_profiles(m: *mut LcMachines) {
    handle_mut!(m, ()).0.refresh_profiles();
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_count(m: *const LcMachines) -> usize {
    handle!(m, 0).0.len()
}

/// The row's machine name, or NULL for no such row. Free with
/// `lc_string_free`.
///
/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_name(m: *const LcMachines, row: usize) -> *mut c_char {
    match handle!(m, std::ptr::null_mut()).0.machine(row) {
        Some(machine) => out(machine.name.clone()),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_family(m: *const LcMachines, row: usize) -> *mut c_char {
    match handle!(m, std::ptr::null_mut()).0.machine(row) {
        Some(machine) => out(machine.family.label()),
        None => std::ptr::null_mut(),
    }
}

/// The "Shader" column: the profile's name if the machine names one that
/// still exists, else a raw override's path, else "(default)".
///
/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_shader_label(m: *const LcMachines, row: usize) -> *mut c_char {
    let m = handle!(m, std::ptr::null_mut());
    if row >= m.0.len() {
        return std::ptr::null_mut();
    }
    out(m.0.shader_label_at(row))
}

/// The bundle *directory*.
///
/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_dir(m: *const LcMachines, row: usize) -> *mut c_char {
    match handle!(m, std::ptr::null_mut()).0.dir(row) {
        Some(dir) => out(dir.display().to_string()),
        None => std::ptr::null_mut(),
    }
}

/// The row's `machine.toml` — how every other model here is addressed.
///
/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_bundle_path(m: *const LcMachines, row: usize) -> *mut c_char {
    match handle!(m, std::ptr::null_mut()).0.bundle_path(row) {
        Some(path) => out(path.display().to_string()),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_is_running(m: *const LcMachines, row: usize) -> bool {
    handle!(m, false).0.is_running(row)
}

/// Whether the machine in a bundle directory is up — how a per-machine
/// window, which knows its bundle and not its row, asks.
///
/// # Safety
/// `m` must be a live handle; `dir` a NUL-terminated string or NULL.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_is_running_dir(m: *const LcMachines, dir: *const c_char) -> bool {
    handle!(m, false).0.is_running_dir(Path::new(unsafe { borrow(dir) }))
}

/// Start a machine's player. Returns true on success; either way
/// `*status` (when non-NULL) is set to the line to show, owned by the
/// caller.
///
/// # Safety
/// `m` must be a live handle; `status` NULL or a writable pointer.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_play(m: *mut LcMachines, row: usize, status: *mut *mut c_char) -> bool {
    let m = handle_mut!(m, false);
    let (ok, message) = match m.0.play(row) {
        Ok(s) => (true, s),
        Err(e) => (false, e),
    };
    if !status.is_null() {
        unsafe { *status = out(message) };
    }
    ok
}

/// Reap any player that exited, writing the rows whose running state
/// changed into `rows` (up to `cap`) and returning how many there were.
/// A caller with a row-based view repaints those; one that redraws
/// everything can pass NULL/0 and ignore the count.
///
/// # Safety
/// `m` must be a live handle; `rows` must be NULL or point to at least
/// `cap` `size_t`s.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_reap(m: *mut LcMachines, rows: *mut usize, cap: usize) -> usize {
    let ended = handle_mut!(m, 0).0.reap();
    if !rows.is_null() {
        for (i, row) in ended.iter().take(cap).enumerate() {
            unsafe { *rows.add(i) = *row };
        }
    }
    ended.len()
}

/// Republish the shared shelf to every running machine's drive, so a
/// disc added while a guest is up shows in its own CDSHELF listing.
///
/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_republish_shelf(m: *const LcMachines) {
    handle!(m, ()).0.republish_shelf();
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_library_dir(m: *const LcMachines) -> *mut c_char {
    out(handle!(m, std::ptr::null_mut()).0.library_dir.display().to_string())
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_disc_library_path(m: *const LcMachines) -> *mut c_char {
    out(handle!(m, std::ptr::null_mut()).0.disc_library_path.display().to_string())
}

/// # Safety
/// `m` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_machines_profiles_dir(m: *const LcMachines) -> *mut c_char {
    out(handle!(m, std::ptr::null_mut()).0.profiles_dir.display().to_string())
}

// --- the wizard ------------------------------------------------------

pub struct LcWizard(wizard::Form);

#[no_mangle]
pub extern "C" fn lc_wizard_new() -> *mut LcWizard {
    Box::into_raw(Box::new(LcWizard(wizard::Form::default())))
}

/// # Safety
/// `w` must be NULL or a handle from `lc_wizard_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_free(w: *mut LcWizard) {
    if !w.is_null() {
        drop(unsafe { Box::from_raw(w) });
    }
}

/// The enum orders every index in this section refers to, which are the
/// orders the labels below come back in.
fn family_at(i: usize) -> Family {
    *Family::ALL.get(i).unwrap_or(&Family::Win98)
}

fn index_of<T: PartialEq + Copy>(all: &[T], value: T) -> usize {
    all.iter().position(|v| *v == value).unwrap_or(0)
}

/// The label for one choice in a combo box, or NULL past the end:
/// `kind` is 0 family, 1 acceleration, 2 processor, 3 boot order. Both
/// Rust front ends fill their pickers this way rather than retyping the
/// strings, and so should a third.
#[no_mangle]
pub extern "C" fn lc_wizard_label(kind: u32, index: usize) -> *mut c_char {
    let label = match kind {
        0 => Family::ALL.get(index).map(|v| v.label()),
        1 => Accel::ALL.get(index).map(|v| v.label()),
        2 => CpuSpeed::ALL.get(index).map(|v| v.label()),
        3 => Boot::ALL.get(index).map(|v| v.label()),
        _ => None,
    };
    match label {
        Some(l) => out(l),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_open_fresh(w: *mut LcWizard) {
    handle_mut!(w, ()).0.open_fresh();
}

/// Open on a family, with everything that follows from it.
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_open_new(w: *mut LcWizard, family: usize) {
    handle_mut!(w, ()).0.open_new(family_at(family));
}

/// Open pre-filled from an existing bundle. False leaves the form closed
/// with `lc_wizard_error` saying why.
///
/// # Safety
/// `w` must be a live handle; `bundle_path` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_open_edit(w: *mut LcWizard, bundle_path: *const c_char) -> bool {
    let w = handle_mut!(w, false);
    w.0.open_edit_path(PathBuf::from(unsafe { borrow(bundle_path) }));
    w.0.open
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_is_open(w: *const LcWizard) -> bool {
    handle!(w, false).0.open
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_is_editing(w: *const LcWizard) -> bool {
    handle!(w, false).0.is_editing()
}

/// "New machine" or "Edit machine".
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_title(w: *const LcWizard) -> *mut c_char {
    out(handle!(w, std::ptr::null_mut()).0.title())
}

// The fields with a consequence. There is deliberately no plain setter
// for any of these: `choose_*` is what applies the rule that memory, the
// accelerator, the processor and the NIC follow the family until someone
// picks one.

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_family(w: *const LcWizard) -> usize {
    index_of(&Family::ALL, handle!(w, 0).0.family())
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_choose_family(w: *mut LcWizard, family: usize) {
    handle_mut!(w, ()).0.choose_family(family_at(family));
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_ram_mb(w: *const LcWizard) -> u32 {
    handle!(w, 0).0.ram_mb()
}

/// The family's memory range: a value outside it is clamped rather than
/// refused at save time.
///
/// # Safety
/// `w` must be a live handle; `min`/`max` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_ram_range(w: *const LcWizard, min: *mut u32, max: *mut u32) {
    let range = handle!(w, ()).0.ram_range();
    if !min.is_null() {
        unsafe { *min = *range.start() };
    }
    if !max.is_null() {
        unsafe { *max = *range.end() };
    }
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_choose_ram_mb(w: *mut LcWizard, ram_mb: u32) {
    handle_mut!(w, ()).0.choose_ram_mb(ram_mb);
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_reset_ram(w: *mut LcWizard) {
    handle_mut!(w, ()).0.reset_ram();
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_ram_is_default(w: *const LcWizard) -> bool {
    handle!(w, false).0.ram_is_default()
}

/// The line under the memory field, or "".
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_ram_note(w: *const LcWizard) -> *mut c_char {
    out_opt(handle!(w, std::ptr::null_mut()).0.ram_note())
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_cpu_speed(w: *const LcWizard) -> usize {
    index_of(&CpuSpeed::ALL, handle!(w, 0).0.cpu_speed())
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_choose_cpu_speed(w: *mut LcWizard, cpu_speed: usize) {
    let s = *CpuSpeed::ALL.get(cpu_speed).unwrap_or(&CpuSpeed::Unthrottled);
    handle_mut!(w, ()).0.choose_cpu_speed(s);
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_reset_cpu_speed(w: *mut LcWizard) {
    handle_mut!(w, ()).0.reset_cpu_speed();
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_cpu_speed_is_default(w: *const LcWizard) -> bool {
    handle!(w, false).0.cpu_speed_is_default()
}

/// The lines under the processor picker, newline-separated: what a
/// chosen processor is for, and that it makes the machine emulated.
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_cpu_speed_note(w: *const LcWizard) -> *mut c_char {
    out(handle!(w, std::ptr::null_mut()).0.cpu_speed_notes().join("\n"))
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_accel(w: *const LcWizard) -> usize {
    index_of(&Accel::ALL, handle!(w, 0).0.accel())
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_choose_accel(w: *mut LcWizard, accel: usize) {
    let a = *Accel::ALL.get(accel).unwrap_or(&Accel::Auto);
    handle_mut!(w, ()).0.choose_accel(a);
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_reset_accel(w: *mut LcWizard) {
    handle_mut!(w, ()).0.reset_accel();
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_accel_is_default(w: *const LcWizard) -> bool {
    handle!(w, false).0.accel_is_default()
}

/// What this host can do for acceleration, said next to the picker —
/// "Automatic" otherwise means something invisible. `*warning` (when
/// non-NULL) is set when the note is a warning: KVM was demanded and
/// this host hasn't got it, so the machine will refuse to start.
///
/// # Safety
/// `w` must be a live handle; `warning` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_accel_note(w: *const LcWizard, warning: *mut bool) -> *mut c_char {
    let note = handle!(w, std::ptr::null_mut()).0.accel_note();
    if !warning.is_null() {
        unsafe { *warning = note.warning };
    }
    out(note.text)
}

/// What this host can do for the guest's 3D (ADR-013), said under the
/// acceleration row. NULL for a DOS machine, which has no Direct3D to
/// place. `*warning` (when non-NULL) is set for the one case that runs
/// and disappoints: a software Vulkan driver, where the Direct3D
/// pass-through works but very slowly.
///
/// # Safety
/// `w` must be a live handle; `warning` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_graphics_note(w: *const LcWizard, warning: *mut bool) -> *mut c_char {
    let Some(note) = handle!(w, std::ptr::null_mut()).0.graphics_note() else {
        if !warning.is_null() {
            unsafe { *warning = false };
        }
        return std::ptr::null_mut();
    };
    if !warning.is_null() {
        unsafe { *warning = note.warning };
    }
    out(note.text)
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_have_kvm(w: *const LcWizard) -> bool {
    handle!(w, false).0.have_kvm()
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_network(w: *const LcWizard) -> bool {
    handle!(w, false).0.network()
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_choose_network(w: *mut LcWizard, network: bool) {
    handle_mut!(w, ()).0.choose_network(network);
}

/// The lines under the networking checkbox, newline-separated: that the
/// NAT is outbound-only, and that these are unpatched systems.
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_network_note(w: *const LcWizard) -> *mut c_char {
    out(handle!(w, std::ptr::null_mut()).0.network_notes().join("\n"))
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_boot(w: *const LcWizard) -> usize {
    index_of(&Boot::ALL, handle!(w, 0).0.boot)
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_set_boot(w: *mut LcWizard, boot: usize) {
    let b = *Boot::ALL.get(boot).unwrap_or(&Boot::Auto);
    handle_mut!(w, ()).0.boot = b;
}

/// "Boot from floppy" with no floppy image, or "".
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_boot_note(w: *const LcWizard) -> *mut c_char {
    out_opt(handle!(w, std::ptr::null_mut()).0.boot_note())
}

/// The plain text and flag fields, by name: "name", "disk_path",
/// "install_media", "floppy", "advanced_toml", "shader_profile". One
/// pair of accessors rather than a dozen, because these have no
/// behaviour behind them — a field with a consequence has a `choose_*`
/// above instead, and there is no way to reach one from here.
///
/// # Safety
/// `w` must be a live handle; `field` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_get(w: *const LcWizard, field: *const c_char) -> *mut c_char {
    let f = &handle!(w, std::ptr::null_mut()).0;
    match unsafe { borrow(field) } {
        "name" => out(f.name.clone()),
        "disk_path" => out(f.disk_path.clone()),
        "install_media" => out(f.install_media.clone()),
        "floppy" => out(f.floppy.clone()),
        "advanced_toml" => out(f.advanced_toml.clone()),
        "shader_profile" => out(f.shader_profile.clone().unwrap_or_default()),
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` must be a live handle; `field` and `value` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_set(w: *mut LcWizard, field: *const c_char, value: *const c_char) -> bool {
    let f = &mut handle_mut!(w, false).0;
    let value = unsafe { borrow(value) }.to_string();
    match unsafe { borrow(field) } {
        "name" => f.name = value,
        "disk_path" => f.disk_path = value,
        "install_media" => f.install_media = value,
        "floppy" => f.floppy = value,
        "advanced_toml" => f.advanced_toml = value,
        "shader_profile" => f.shader_profile = Some(value).filter(|v| !v.is_empty()),
        _ => return false,
    }
    true
}

/// The three booleans and the disk size, by name: "existing_disk",
/// "advanced" for the flags, "disk_size_gb" for the number.
///
/// # Safety
/// `w` must be a live handle; `field` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_get_flag(w: *const LcWizard, field: *const c_char) -> bool {
    let f = &handle!(w, false).0;
    match unsafe { borrow(field) } {
        "existing_disk" => f.existing_disk,
        "advanced" => f.advanced,
        _ => false,
    }
}

/// # Safety
/// `w` must be a live handle; `field` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_set_flag(w: *mut LcWizard, field: *const c_char, value: bool) -> bool {
    let f = &mut handle_mut!(w, false).0;
    match unsafe { borrow(field) } {
        "existing_disk" => f.existing_disk = value,
        "advanced" => f.advanced = value,
        _ => return false,
    }
    true
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_disk_size_gb(w: *const LcWizard) -> u32 {
    handle!(w, 0).0.disk_size_gb
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_set_disk_size_gb(w: *mut LcWizard, gb: u32) {
    handle_mut!(w, ()).0.disk_size_gb = gb.max(1);
}

/// Fill the advanced box if it is still empty: the file's exact current
/// text when editing, the TOML this form describes when creating.
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_fill_advanced(w: *mut LcWizard) {
    handle_mut!(w, ()).0.fill_advanced();
}

/// Create or save the machine into `library_dir` (NULL or "" for the
/// user's own). True closes the form; false leaves it open with
/// `lc_wizard_error` saying why.
///
/// # Safety
/// `w` must be a live handle; `library_dir` NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_submit(w: *mut LcWizard, library_dir: *const c_char) -> bool {
    let dir = unsafe { borrow(library_dir) };
    let dir = if dir.is_empty() {
        launcher_core::library::default_dir()
    } else {
        PathBuf::from(dir)
    };
    handle_mut!(w, false).0.submit(&dir).is_some()
}

/// The `machine.toml` the last successful submit wrote, or "".
///
/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_saved_path(w: *const LcWizard) -> *mut c_char {
    out(handle!(w, std::ptr::null_mut()).0.saved_path().map(|p| p.display().to_string()).unwrap_or_default())
}

/// # Safety
/// `w` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_wizard_error(w: *const LcWizard) -> *mut c_char {
    out_opt(handle!(w, std::ptr::null_mut()).0.error.as_deref())
}

// --- the disc shelf --------------------------------------------------

pub struct LcShelf(shelf::Shelf);

#[no_mangle]
pub extern "C" fn lc_shelf_new() -> *mut LcShelf {
    Box::into_raw(Box::new(LcShelf(shelf::Shelf::default())))
}

/// # Safety
/// `s` must be NULL or a handle from `lc_shelf_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_free(s: *mut LcShelf) {
    if !s.is_null() {
        drop(unsafe { Box::from_raw(s) });
    }
}

/// Open on the shared shelf alone.
///
/// # Safety
/// `s` must be a live handle; `library_path` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_open_library(s: *mut LcShelf, library_path: *const c_char) {
    let path = PathBuf::from(unsafe { borrow(library_path) });
    handle_mut!(s, ()).0.open_library(&path);
}

/// Open for one machine: the same shelf, plus its boot-disc choice and
/// (while running) live insert.
///
/// # Safety
/// `s` must be a live handle; both paths NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_open_for(s: *mut LcShelf, bundle_path: *const c_char, library_path: *const c_char) {
    let bundle = PathBuf::from(unsafe { borrow(bundle_path) });
    let library = PathBuf::from(unsafe { borrow(library_path) });
    handle_mut!(s, ()).0.open_for_path(bundle, &library);
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_count(s: *const LcShelf) -> usize {
    handle!(s, 0).0.discs().len()
}

/// A row's label (the editable one), or NULL past the end.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_label(s: *const LcShelf, row: usize) -> *mut c_char {
    match handle!(s, std::ptr::null_mut()).0.discs().get(row) {
        Some(disc) => out(disc.label.clone()),
        None => std::ptr::null_mut(),
    }
}

/// A row's image path.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_path(s: *const LcShelf, row: usize) -> *mut c_char {
    match handle!(s, std::ptr::null_mut()).0.discs().get(row) {
        Some(disc) => out(disc.path.display().to_string()),
        None => std::ptr::null_mut(),
    }
}

/// Whether this row is the open machine's boot disc.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_is_boot(s: *const LcShelf, row: usize) -> bool {
    let s = handle!(s, false);
    match s.0.discs().get(row) {
        Some(disc) => s.0.boot() == Some(disc.path.as_path()),
        None => false,
    }
}

/// Rename a row. The write is `lc_shelf_flush`, so a field being typed
/// into does not save the file on every keystroke.
///
/// # Safety
/// `s` must be a live handle; `label` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_set_label(s: *mut LcShelf, row: usize, label: *const c_char) {
    let label = unsafe { borrow(label) }.to_string();
    handle_mut!(s, ()).0.set_label(row, &label);
}

/// # Safety
/// `s` must be a live handle; `path` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_add(s: *mut LcShelf, path: *const c_char) {
    let path = PathBuf::from(unsafe { borrow(path) });
    handle_mut!(s, ()).0.add(path);
}

/// Doc 07's one-click attach: the guest-tools ISO this checkout built.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_add_guest_tools(s: *mut LcShelf) {
    handle_mut!(s, ()).0.add_guest_tools();
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_remove(s: *mut LcShelf, row: usize) {
    handle_mut!(s, ()).0.remove_row(row);
}

/// Make a row the open machine's boot disc, writing the bundle.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_set_boot(s: *mut LcShelf, row: usize) -> bool {
    handle_mut!(s, false).0.set_boot_row(row).is_some()
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_clear_boot(s: *mut LcShelf) -> bool {
    handle_mut!(s, false).0.set_boot(None).is_some()
}

/// The label for what the machine boots with, "(empty tray)" if nothing.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_boot_label(s: *const LcShelf) -> *mut c_char {
    out(handle!(s, std::ptr::null_mut()).0.boot_label())
}

/// Swap a row's disc into the running machine's drive now.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_insert_live(s: *mut LcShelf, row: usize) {
    handle_mut!(s, ()).0.insert_live_row(row);
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_eject_live(s: *mut LcShelf) {
    handle_mut!(s, ()).0.eject_live();
}

/// Write pending edits. Failures land in `lc_shelf_error`.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_flush(s: *mut LcShelf) {
    handle_mut!(s, ()).0.flush_reporting();
}

/// Whether the shelf was written since this was last asked — the cue to
/// call `lc_machines_republish_shelf`.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_take_saved(s: *mut LcShelf) -> bool {
    handle_mut!(s, false).0.take_saved()
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_title(s: *const LcShelf) -> *mut c_char {
    out(handle!(s, std::ptr::null_mut()).0.title())
}

/// Whether a machine's context is loaded (the boot column and the live
/// row only exist then).
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_for_machine(s: *const LcShelf) -> bool {
    handle!(s, false).0.for_machine()
}

/// The bundle directory the shelf has open, or "".
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_bundle_dir(s: *const LcShelf) -> *mut c_char {
    out(handle!(s, std::ptr::null_mut()).0.bundle_dir().map(|d| d.display().to_string()).unwrap_or_default())
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_status(s: *const LcShelf) -> *mut c_char {
    out_opt(handle!(s, std::ptr::null_mut()).0.status())
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_shelf_error(s: *const LcShelf) -> *mut c_char {
    out_opt(handle!(s, std::ptr::null_mut()).0.error())
}

// --- snapshots -------------------------------------------------------

pub struct LcSnapshots(snaps::Snapshots);

#[no_mangle]
pub extern "C" fn lc_snapshots_new() -> *mut LcSnapshots {
    Box::into_raw(Box::new(LcSnapshots(snaps::Snapshots::default())))
}

/// # Safety
/// `s` must be NULL or a handle from `lc_snapshots_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_free(s: *mut LcSnapshots) {
    if !s.is_null() {
        drop(unsafe { Box::from_raw(s) });
    }
}

/// Open on a bundle. `running` says whether that machine's player is up:
/// if it is, every operation goes through the machine's monitor, because
/// `qemu-img` writing to an image QEMU has open corrupts it.
///
/// # Safety
/// `s` must be a live handle; `bundle_path` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_open_for(s: *mut LcSnapshots, bundle_path: *const c_char, running: bool) {
    let path = PathBuf::from(unsafe { borrow(bundle_path) });
    handle_mut!(s, ()).0.open_for_path(&path, running);
}

/// The machine started or stopped under the window: the list now has to
/// come from the other source.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_set_running(s: *mut LcSnapshots, running: bool) {
    handle_mut!(s, ()).0.set_running(running);
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_count(s: *const LcSnapshots) -> usize {
    handle!(s, 0).0.snapshots().len()
}

/// A row's tag, or NULL past the end.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_name(s: *const LcSnapshots, row: usize) -> *mut c_char {
    match handle!(s, std::ptr::null_mut()).0.snapshots().get(row) {
        Some(snap) => out(snap.name.clone()),
        None => std::ptr::null_mut(),
    }
}

/// When it was taken, formatted.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_date_label(s: *const LcSnapshots, row: usize) -> *mut c_char {
    match handle!(s, std::ptr::null_mut()).0.snapshots().get(row) {
        Some(snap) => out(snap.date_label()),
        None => std::ptr::null_mut(),
    }
}

/// The size of the saved CPU/RAM state, or "—" for a disk-only snapshot.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_size_label(s: *const LcSnapshots, row: usize) -> *mut c_char {
    match handle!(s, std::ptr::null_mut()).0.snapshots().get(row) {
        Some(snap) => out(snap.size_label()),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `s` must be a live handle; `name` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_take(s: *mut LcSnapshots, name: *const c_char) {
    let name = unsafe { borrow(name) }.to_string();
    handle_mut!(s, ()).0.take(&name);
}

/// Restore. Destructive and with no undo: a front end should confirm.
///
/// # Safety
/// `s` must be a live handle; `name` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_revert(s: *mut LcSnapshots, name: *const c_char) {
    let name = unsafe { borrow(name) }.to_string();
    handle_mut!(s, ()).0.revert(&name);
}

/// # Safety
/// `s` must be a live handle; `name` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_delete(s: *mut LcSnapshots, name: *const c_char) {
    let name = unsafe { borrow(name) }.to_string();
    handle_mut!(s, ()).0.drop_snapshot(&name);
}

/// Whether a live job is in flight: disable the buttons and keep
/// polling. Live save/load/delete are QMP *jobs* — saving a 512 MB
/// guest's RAM takes a visible moment — so nothing here blocks on one.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_job_pending(s: *const LcSnapshots) -> bool {
    handle!(s, false).0.job_pending()
}

/// Check an in-flight job, throttled to a couple of times a second
/// however often it is called — safe from a display link or a 100 ms
/// timer alike.
///
/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_poll(s: *mut LcSnapshots) {
    handle_mut!(s, ()).0.poll_job();
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_title(s: *const LcSnapshots) -> *mut c_char {
    out(handle!(s, std::ptr::null_mut()).0.title())
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_status(s: *const LcSnapshots) -> *mut c_char {
    out_opt(handle!(s, std::ptr::null_mut()).0.status())
}

/// # Safety
/// `s` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_snapshots_error(s: *const LcSnapshots) -> *mut c_char {
    out_opt(handle!(s, std::ptr::null_mut()).0.error())
}

// --- the shader profile editor ---------------------------------------

pub struct LcEditor {
    editor: editor::Editor,
    presets: editor::Presets,
    preview: Option<preview::Preview>,
}

#[no_mangle]
pub extern "C" fn lc_editor_new() -> *mut LcEditor {
    Box::into_raw(Box::new(LcEditor {
        editor: editor::Editor::default(),
        presets: editor::Presets::default(),
        preview: None,
    }))
}

/// # Safety
/// `e` must be NULL or a handle from `lc_editor_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_free(e: *mut LcEditor) {
    if !e.is_null() {
        drop(unsafe { Box::from_raw(e) });
    }
}

/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_new_profile(e: *mut LcEditor) {
    handle_mut!(e, ()).editor.new_profile();
}

/// Open an existing profile.
///
/// # Safety
/// `e` must be a live handle; `path` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_edit(e: *mut LcEditor, path: *const c_char) {
    let path = PathBuf::from(unsafe { borrow(path) });
    handle_mut!(e, ()).editor.edit_path(path);
}

/// The editor's own text fields: "name", "preset_path", "preview_image".
///
/// # Safety
/// `e` must be a live handle; `field` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_get(e: *const LcEditor, field: *const c_char) -> *mut c_char {
    let ed = &handle!(e, std::ptr::null_mut()).editor;
    match unsafe { borrow(field) } {
        "name" => out(ed.name.clone()),
        "preset_path" => out(ed.preset_path.clone()),
        "preview_image" => out(ed.preview_image_path.clone()),
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// `e` must be a live handle; `field` and `value` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_set(e: *mut LcEditor, field: *const c_char, value: *const c_char) -> bool {
    let value = unsafe { borrow(value) }.to_string();
    let ed = &mut handle_mut!(e, false).editor;
    match unsafe { borrow(field) } {
        "name" => ed.name = value,
        "preset_path" => ed.preset_path = value,
        "preview_image" => ed.preview_image_path = value,
        _ => return false,
    }
    true
}

/// Re-read the preset's parameters if the path changed. Cheap to call
/// whenever the field is committed, or once a frame.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_reparse(e: *mut LcEditor) {
    handle_mut!(e, ()).editor.reparse();
}

/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_param_count(e: *const LcEditor) -> usize {
    handle!(e, 0).editor.params().len()
}

/// A parameter's id, or NULL past the end.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_param_id(e: *const LcEditor, row: usize) -> *mut c_char {
    match handle!(e, std::ptr::null_mut()).editor.params().get(row) {
        Some(meta) => out(meta.id.clone()),
        None => std::ptr::null_mut(),
    }
}

/// The description worth showing under a row — "" when it merely repeats
/// the id.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_param_description(e: *const LcEditor, row: usize) -> *mut c_char {
    let e = handle!(e, std::ptr::null_mut());
    if row >= e.editor.params().len() {
        return std::ptr::null_mut();
    }
    out_opt(e.editor.description(row))
}

/// A parameter's slider bounds and values. Any output pointer may be
/// NULL. False for no such row.
///
/// # Safety
/// `e` must be a live handle; each non-NULL out pointer must be writable.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_param_range(
    e: *const LcEditor,
    row: usize,
    minimum: *mut f32,
    maximum: *mut f32,
    step: *mut f32,
    default: *mut f32,
    value: *mut f32,
    overridden: *mut bool,
) -> bool {
    let e = handle!(e, false);
    let Some((meta, over)) = e.editor.param(row) else { return false };
    unsafe {
        if !minimum.is_null() {
            *minimum = meta.minimum;
        }
        if !maximum.is_null() {
            *maximum = meta.maximum;
        }
        if !step.is_null() {
            *step = meta.step;
        }
        if !default.is_null() {
            *default = meta.default;
        }
        if !value.is_null() {
            *value = over.unwrap_or(meta.default);
        }
        if !overridden.is_null() {
            *overridden = over.is_some();
        }
    }
    true
}

/// Override (or stop overriding) one parameter. Starting to override
/// seeds the preset's own default, so the slider doesn't jump.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_set_override(e: *mut LcEditor, row: usize, enabled: bool) {
    handle_mut!(e, ()).editor.set_override(row, enabled);
}

/// Move an overridden parameter. **Ignored for a row that isn't
/// overridden**, which is the guard that stops a disabled slider's
/// step-snapped value from silently becoming an override.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_set_value(e: *mut LcEditor, row: usize, value: f32) {
    handle_mut!(e, ()).editor.set_value(row, value);
}

/// Whether there is anything the preview could render yet.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_renderable(e: *const LcEditor) -> bool {
    handle!(e, false).editor.renderable()
}

/// Render one preview frame into an `area_w` x `area_h` box, then report
/// the size it actually came out at (the image's own size times the
/// largest integer scale that fits — never a fraction, so the shader is
/// not blurred by a second resample). Centre that on black. False when
/// there was nothing to render; `lc_editor_error` says why if anything
/// failed.
///
/// The first call opens a windowless GPU device of the editor's own,
/// which is what a front end whose toolkit will not lend one needs. Read
/// the frame with `lc_editor_read_frame`.
///
/// # Safety
/// `e` must be a live handle; `out_w`/`out_h` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_render(
    e: *mut LcEditor,
    area_w: u32,
    area_h: u32,
    out_w: *mut u32,
    out_h: *mut u32,
) -> bool {
    let e = handle_mut!(e, false);
    if !e.editor.renderable() {
        return false;
    }
    if e.preview.is_none() {
        match preview::Preview::headless() {
            Ok(p) => e.preview = Some(p),
            Err(err) => {
                e.editor.error = Some(err);
                return false;
            }
        }
    }
    let params = e.editor.effective();
    let preset = PathBuf::from(e.editor.preset_path.trim());
    let image = PathBuf::from(e.editor.preview_image_path.trim());
    let preview = e.preview.as_mut().expect("just created");
    preview.update(&preset, &params, &image, area_w.max(1), area_h.max(1));
    let (w, h) = preview.viewport();
    unsafe {
        if !out_w.is_null() {
            *out_w = w;
        }
        if !out_h.is_null() {
            *out_h = h;
        }
    }
    match preview.error() {
        Some(err) => {
            e.editor.error = Some(err.to_string());
            false
        }
        None => true,
    }
}

/// Copy the last rendered frame into `buf` as RGB8, row-major, top-down:
/// `width * height * 3` bytes. Returns the number of bytes the frame
/// needs, so a caller may pass NULL/0 first to size its buffer; nothing
/// is written if `cap` is short.
///
/// # Safety
/// `e` must be a live handle; `buf` NULL or writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_read_frame(e: *const LcEditor, buf: *mut u8, cap: usize) -> usize {
    let e = handle!(e, 0);
    let Some(preview) = e.preview.as_ref() else { return 0 };
    let Some((_, _, rgb)) = preview.read_frame() else { return 0 };
    if !buf.is_null() && cap >= rgb.len() {
        unsafe { std::ptr::copy_nonoverlapping(rgb.as_ptr(), buf, rgb.len()) };
    }
    rgb.len()
}

/// Write the profile into `profiles_dir` (NULL or "" for the user's
/// own). A *new* profile keeps the overrides it collected.
///
/// # Safety
/// `e` must be a live handle; `profiles_dir` NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_save(e: *mut LcEditor, profiles_dir: *const c_char) -> bool {
    let dir = unsafe { borrow(profiles_dir) };
    let dir = if dir.is_empty() {
        launcher_core::shader_library::default_dir()
    } else {
        PathBuf::from(dir)
    };
    handle_mut!(e, false).editor.save(&dir)
}

/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_parse_error(e: *const LcEditor) -> *mut c_char {
    out_opt(handle!(e, std::ptr::null_mut()).editor.parse_error())
}

/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_error(e: *const LcEditor) -> *mut c_char {
    out_opt(handle!(e, std::ptr::null_mut()).editor.error.as_deref())
}

/// The preset collection's state, for the row both shader screens show:
/// 0 ready, 1 missing, 2 downloading, 3 failed. `detail` (when non-NULL)
/// receives the directory, the install directory, the megabytes so far,
/// or the failure — owned by the caller.
///
/// Safe to call as often as you like: it is also what advances a
/// finished download into the cached directory.
///
/// # Safety
/// `e` must be a live handle; `detail` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_preset_state(e: *mut LcEditor, detail: *mut *mut c_char) -> u32 {
    let e = handle_mut!(e, 1);
    let (code, text) = match e.presets.state() {
        editor::PresetState::Ready(dir) => (0, dir.display().to_string()),
        editor::PresetState::Missing { install_dir, size } => {
            (1, format!("{} ({size})", install_dir.display()))
        }
        editor::PresetState::Downloading(mb) => (2, format!("{mb:.1}")),
        editor::PresetState::Failed(err) => (3, err),
    };
    if !detail.is_null() {
        unsafe { *detail = out(text) };
    }
    code
}

/// Start fetching the upstream preset collection. It runs on its own
/// thread; watch it with `lc_editor_preset_state`.
///
/// # Safety
/// `e` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn lc_editor_download_presets(e: *mut LcEditor) {
    handle_mut!(e, ()).presets.start_download();
}

// --- paths -----------------------------------------------------------

/// Where something lives, by name: "player", "qemu_img", "pc_bios",
/// "machines", "discs", "profiles", "shaders", "guest_tools". Empty for
/// the two that can legitimately be absent (a preset collection that has
/// not been downloaded, a guest-tools ISO that has not been built), NULL
/// for an unknown name.
///
/// The rule behind all of them is that everything is relative to the
/// running executable — so a front end that links this must expect these
/// to resolve against *its* binary, not against a checkout.
///
/// # Safety
/// `what` must be NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn lc_path(what: *const c_char) -> *mut c_char {
    use launcher_core::{disc_library, library, player, shader_library, shader_source};
    let path = match unsafe { borrow(what) } {
        "player" => player::player_binary(),
        "qemu_img" => player::qemu_img_binary(),
        "pc_bios" => player::pc_bios_dir(),
        "machines" => library::default_dir(),
        "discs" => disc_library::default_path(),
        "profiles" => shader_library::default_dir(),
        "shaders" => return out(shader_source::presets_dir().map(|d| d.display().to_string()).unwrap_or_default()),
        "guest_tools" => {
            return out(disc_library::guest_tools_iso().map(|p| p.display().to_string()).unwrap_or_default())
        }
        _ => return std::ptr::null_mut(),
    };
    out(path.display().to_string())
}

/// Whether this host can give a guest KVM — the same answer the wizard's
/// acceleration hint reads.
#[no_mangle]
pub extern "C" fn lc_kvm_available() -> bool {
    launcher_core::player::kvm_available()
}
