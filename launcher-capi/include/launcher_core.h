/* The 2ksbox launcher, as a C library (doc 07).
 *
 * Everything the launcher does that is not drawing lives in one Rust
 * crate (`launcher-core`), and the project's two front ends — the egui
 * one and the Qt/QML one — are views over it. This header is the same
 * thing for a front end that is not Rust: a native macOS app in Swift is
 * the case it was shaped for (Swift imports a C header directly, with no
 * bridge), but anything that speaks C works.
 *
 * Link `liblauncher_capi.a` (or the .so/.dylib) from
 * `cargo build -p launcher-capi`.
 *
 *   Every window is an opaque handle: lc_*_new / lc_*_free.
 *   Rows are addressed by index and read one field at a time.
 *   Every char * returned here is yours: free it with lc_string_free.
 *     A getter never returns NULL for "empty" — it returns "" — so NULL
 *     means only "no such row" (or, for lc_path, "no such name").
 *   Every char * passed in is borrowed, must be UTF-8, and is copied
 *     before the call returns.
 *   A handle is not thread-safe: use one from one thread at a time.
 *   Nothing here blocks on a guest. The two long operations have polls:
 *     lc_snapshots_poll while lc_snapshots_job_pending, and
 *     lc_editor_preset_state while a download runs.
 *
 * What a front end still owes, because it is genuinely the toolkit's:
 * a file dialog, when to redraw, how to confirm a destructive restore,
 * and how to show a preview frame (lc_editor_read_frame hands you RGB8).
 */
#ifndef LAUNCHER_CORE_H
#define LAUNCHER_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lc_string_free(char *s);

/* Where a companion lives, by name: "player", "qemu_img", "pc_bios",
 * "machines", "discs", "profiles", "shaders", "guest_tools". Everything
 * resolves relative to the *running executable*, so these answer about
 * your binary, not about a checkout. NULL for an unknown name. */
char *lc_path(const char *what);
bool lc_kvm_available(void);

/* --- the machine library ------------------------------------------ */

typedef struct LcMachines LcMachines;

LcMachines *lc_machines_new(void);
void lc_machines_free(LcMachines *m);
void lc_machines_refresh(LcMachines *m);
/* After the profile editor saved or deleted one: the "Shader" column
 * moves, the rows do not. */
void lc_machines_refresh_profiles(LcMachines *m);
size_t lc_machines_count(const LcMachines *m);
char *lc_machines_name(const LcMachines *m, size_t row);
char *lc_machines_family(const LcMachines *m, size_t row);
char *lc_machines_shader_label(const LcMachines *m, size_t row);
char *lc_machines_dir(const LcMachines *m, size_t row);
/* The row's machine.toml: how every other model here is addressed. */
char *lc_machines_bundle_path(const LcMachines *m, size_t row);
bool lc_machines_is_running(const LcMachines *m, size_t row);
bool lc_machines_is_running_dir(const LcMachines *m, const char *dir);
/* Publishes the shelf to the machine's drive, derives its monitor socket
 * from the bundle directory, and spawns the player. *status (if given)
 * is the line to show, either way. */
bool lc_machines_play(LcMachines *m, size_t row, char **status);
/* Reap exited players; writes the rows whose running state changed into
 * `rows` (up to `cap`) and returns how many there were. A child process
 * cannot push this news, so call it from a timer. */
size_t lc_machines_reap(LcMachines *m, size_t *rows, size_t cap);
/* After lc_shelf_take_saved: a disc added while a guest is up should
 * show in its own in-guest CDSHELF listing without a restart. */
void lc_machines_republish_shelf(const LcMachines *m);
char *lc_machines_library_dir(const LcMachines *m);
char *lc_machines_disc_library_path(const LcMachines *m);
char *lc_machines_profiles_dir(const LcMachines *m);

/* --- the guided creation form -------------------------------------- */

typedef struct LcWizard LcWizard;

/* The combo boxes' labels, in the order every index below uses. Fill a
 * picker by walking `index` until this returns NULL, rather than
 * retyping the strings — both Rust front ends do exactly this. */
#define LC_LABEL_FAMILY    0u
#define LC_LABEL_ACCEL     1u
#define LC_LABEL_CPU_SPEED 2u
#define LC_LABEL_BOOT      3u
char *lc_wizard_label(uint32_t kind, size_t index);

LcWizard *lc_wizard_new(void);
void lc_wizard_free(LcWizard *w);
void lc_wizard_open_fresh(LcWizard *w);
void lc_wizard_open_new(LcWizard *w, size_t family);
bool lc_wizard_open_edit(LcWizard *w, const char *bundle_path);
bool lc_wizard_is_open(const LcWizard *w);
bool lc_wizard_is_editing(const LcWizard *w);
char *lc_wizard_title(const LcWizard *w);

/* The fields with a consequence. There is deliberately no plain setter
 * for any of them: choose_* is what applies the rule that memory, the
 * accelerator, the processor and the network card follow the family
 * until someone picks one — and having no other way in is what makes
 * that rule impossible to forget in a new widget. */
size_t lc_wizard_family(const LcWizard *w);
void lc_wizard_choose_family(LcWizard *w, size_t family);

uint32_t lc_wizard_ram_mb(const LcWizard *w);
void lc_wizard_ram_range(const LcWizard *w, uint32_t *min, uint32_t *max);
void lc_wizard_choose_ram_mb(LcWizard *w, uint32_t ram_mb);
void lc_wizard_reset_ram(LcWizard *w);
bool lc_wizard_ram_is_default(const LcWizard *w);
char *lc_wizard_ram_note(const LcWizard *w);

size_t lc_wizard_cpu_speed(const LcWizard *w);
void lc_wizard_choose_cpu_speed(LcWizard *w, size_t cpu_speed);
void lc_wizard_reset_cpu_speed(LcWizard *w);
bool lc_wizard_cpu_speed_is_default(const LcWizard *w);
/* Newline-separated. */
char *lc_wizard_cpu_speed_note(const LcWizard *w);

size_t lc_wizard_accel(const LcWizard *w);
void lc_wizard_choose_accel(LcWizard *w, size_t accel);
void lc_wizard_reset_accel(LcWizard *w);
bool lc_wizard_accel_is_default(const LcWizard *w);
/* *warning is set when this is a warning rather than a note: KVM was
 * demanded and this host hasn't got it, so the machine won't start. */
char *lc_wizard_accel_note(const LcWizard *w, bool *warning);
bool lc_wizard_have_kvm(const LcWizard *w);

bool lc_wizard_network(const LcWizard *w);
void lc_wizard_choose_network(LcWizard *w, bool network);
/* Newline-separated. */
char *lc_wizard_network_note(const LcWizard *w);

size_t lc_wizard_boot(const LcWizard *w);
void lc_wizard_set_boot(LcWizard *w, size_t boot);
char *lc_wizard_boot_note(const LcWizard *w);

/* The plain fields, by name — one pair of accessors rather than a dozen,
 * because there is no behaviour behind them.
 *   text:  "name" "disk_path" "install_media" "floppy" "advanced_toml"
 *          "shader_profile"
 *   flags: "existing_disk" "advanced"                                  */
char *lc_wizard_get(const LcWizard *w, const char *field);
bool lc_wizard_set(LcWizard *w, const char *field, const char *value);
bool lc_wizard_get_flag(const LcWizard *w, const char *field);
bool lc_wizard_set_flag(LcWizard *w, const char *field, bool value);
uint32_t lc_wizard_disk_size_gb(const LcWizard *w);
void lc_wizard_set_disk_size_gb(LcWizard *w, uint32_t gb);

/* Fill the advanced box if empty: the file's exact text when editing,
 * the TOML this form describes when creating. */
void lc_wizard_fill_advanced(LcWizard *w);
/* library_dir may be NULL or "" for the user's own. False leaves the
 * form open with lc_wizard_error saying why. */
bool lc_wizard_submit(LcWizard *w, const char *library_dir);
char *lc_wizard_saved_path(const LcWizard *w);
char *lc_wizard_error(const LcWizard *w);

/* --- the disc shelf ------------------------------------------------- */

typedef struct LcShelf LcShelf;

LcShelf *lc_shelf_new(void);
void lc_shelf_free(LcShelf *s);
/* The shared collection alone. */
void lc_shelf_open_library(LcShelf *s, const char *library_path);
/* The same collection, plus one machine's boot-disc choice and (while it
 * runs) live insert. */
void lc_shelf_open_for(LcShelf *s, const char *bundle_path, const char *library_path);
size_t lc_shelf_count(const LcShelf *s);
char *lc_shelf_label(const LcShelf *s, size_t row);
char *lc_shelf_path(const LcShelf *s, size_t row);
bool lc_shelf_is_boot(const LcShelf *s, size_t row);
/* Renaming is not saving: call lc_shelf_flush when the edit is
 * finished, so a field being typed into doesn't write the file on every
 * keystroke. */
void lc_shelf_set_label(LcShelf *s, size_t row, const char *label);
void lc_shelf_add(LcShelf *s, const char *path);
void lc_shelf_add_guest_tools(LcShelf *s);
void lc_shelf_remove(LcShelf *s, size_t row);
bool lc_shelf_set_boot(LcShelf *s, size_t row);
bool lc_shelf_clear_boot(LcShelf *s);
char *lc_shelf_boot_label(const LcShelf *s);
void lc_shelf_insert_live(LcShelf *s, size_t row);
void lc_shelf_eject_live(LcShelf *s);
void lc_shelf_flush(LcShelf *s);
bool lc_shelf_take_saved(LcShelf *s);
char *lc_shelf_title(const LcShelf *s);
bool lc_shelf_for_machine(const LcShelf *s);
char *lc_shelf_bundle_dir(const LcShelf *s);
char *lc_shelf_status(const LcShelf *s);
char *lc_shelf_error(const LcShelf *s);

/* --- snapshots ------------------------------------------------------ */

typedef struct LcSnapshots LcSnapshots;

LcSnapshots *lc_snapshots_new(void);
void lc_snapshots_free(LcSnapshots *s);
/* `running` decides everything: a running machine is driven through its
 * monitor, because qemu-img writing to an image QEMU has open corrupts
 * it and even listing wants a lock QEMU already holds. */
void lc_snapshots_open_for(LcSnapshots *s, const char *bundle_path, bool running);
void lc_snapshots_set_running(LcSnapshots *s, bool running);
size_t lc_snapshots_count(const LcSnapshots *s);
char *lc_snapshots_name(const LcSnapshots *s, size_t row);
char *lc_snapshots_date_label(const LcSnapshots *s, size_t row);
char *lc_snapshots_size_label(const LcSnapshots *s, size_t row);
void lc_snapshots_take(LcSnapshots *s, const char *name);
/* Destructive, with no undo: confirm it. */
void lc_snapshots_revert(LcSnapshots *s, const char *name);
void lc_snapshots_delete(LcSnapshots *s, const char *name);
/* A live save/load/delete is a QMP *job*: it returns as soon as the job
 * exists and finishes later. Disable the buttons while pending and call
 * poll from a timer; poll throttles itself, so calling it often is free. */
bool lc_snapshots_job_pending(const LcSnapshots *s);
void lc_snapshots_poll(LcSnapshots *s);
char *lc_snapshots_title(const LcSnapshots *s);
char *lc_snapshots_status(const LcSnapshots *s);
char *lc_snapshots_error(const LcSnapshots *s);

/* --- the shader profile editor -------------------------------------- */

typedef struct LcEditor LcEditor;

LcEditor *lc_editor_new(void);
void lc_editor_free(LcEditor *e);
void lc_editor_new_profile(LcEditor *e);
void lc_editor_edit(LcEditor *e, const char *path);
/* Text fields: "name" "preset_path" "preview_image". */
char *lc_editor_get(const LcEditor *e, const char *field);
bool lc_editor_set(LcEditor *e, const char *field, const char *value);
void lc_editor_reparse(LcEditor *e);
size_t lc_editor_param_count(const LcEditor *e);
char *lc_editor_param_id(const LcEditor *e, size_t row);
/* "" when the description merely repeats the id. */
char *lc_editor_param_description(const LcEditor *e, size_t row);
/* Any out pointer may be NULL. */
bool lc_editor_param_range(const LcEditor *e, size_t row, float *minimum, float *maximum,
                           float *step, float *default_value, float *value, bool *overridden);
void lc_editor_set_override(LcEditor *e, size_t row, bool enabled);
/* Ignored for a row that isn't overridden — the guard that stops a
 * disabled slider's step-snapped value from becoming an override, which
 * several presets' own defaults would otherwise trigger just by being
 * opened (crt-lottes: warpX 0.031, step 0.01). */
void lc_editor_set_value(LcEditor *e, size_t row, float value);

bool lc_editor_renderable(const LcEditor *e);
/* Renders into an area and reports the size the frame actually came out
 * at: the image's own size times the largest *integer* scale that fits,
 * never a fraction. Centre that on black — that is how the player shows
 * it. The first call opens a windowless GPU device of its own. */
bool lc_editor_render(LcEditor *e, uint32_t area_w, uint32_t area_h,
                      uint32_t *out_w, uint32_t *out_h);
/* The last frame as RGB8, row-major, top-down. Returns the bytes needed,
 * so NULL/0 sizes your buffer; nothing is written if cap is short. */
size_t lc_editor_read_frame(const LcEditor *e, uint8_t *buf, size_t cap);
/* profiles_dir may be NULL or "" for the user's own. A *new* profile
 * keeps the overrides the editor collected. */
bool lc_editor_save(LcEditor *e, const char *profiles_dir);
char *lc_editor_parse_error(const LcEditor *e);
char *lc_editor_error(const LcEditor *e);

/* The preset collection's state, for the row both shader screens show.
 * *detail (if given) is the directory / the install directory and size /
 * the megabytes so far / the failure. Calling this is also what advances
 * a finished download, so call it freely. */
#define LC_PRESETS_READY       0u
#define LC_PRESETS_MISSING     1u
#define LC_PRESETS_DOWNLOADING 2u
#define LC_PRESETS_FAILED      3u
uint32_t lc_editor_preset_state(LcEditor *e, char **detail);
void lc_editor_download_presets(LcEditor *e);

#ifdef __cplusplus
}
#endif
#endif /* LAUNCHER_CORE_H */
