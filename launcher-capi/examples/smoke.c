/* A third front end, in the smallest possible form: a C program that
 * drives the same models the egui and Qt builds do, through
 * `include/launcher_core.h`.
 *
 * It is a *test*, not a demo — `scripts/test.sh host` builds and runs it
 * against a scratch library — and what it tests is that the C ABI is
 * still whole and still means the same thing as the Rust one. It creates
 * a DOS machine through the wizard and checks the answers the shared
 * form gives: 64 MB, a period processor, emulated because a throttle
 * needs TCG, no network card. Then a disc onto the shelf, the machine
 * seen from the library, and the shelf read back.
 *
 * Everything it prints is checked by the caller, so a change that
 * silently alters one of those defaults fails here as well as in the two
 * GUIs.
 *
 * Build:
 *   cargo build -p launcher-capi
 *   cc -I launcher-capi/include launcher-capi/examples/smoke.c \
 *      target/debug/liblauncher_capi.a -lm -ldl -lpthread -o smoke
 */
#include "launcher_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int ok, const char *saw) {
    printf("  %s %-52s %s\n", ok ? "PASS" : "FAIL", what, saw ? saw : "");
    if (!ok) failures++;
}

/* Every getter hands over a string the caller owns; this checks one and
 * frees it in the same breath, which is also the usage pattern a real
 * front end wants. */
static void check_str(const char *what, char *got, const char *want) {
    check(what, got && strcmp(got, want) == 0, got);
    lc_string_free(got);
}

/* The index of a label in one of the wizard's pickers, or -1. A front
 * end fills a combo box this way rather than hard-coding the strings. */
static long label_index(uint32_t kind, const char *want) {
    for (size_t i = 0;; i++) {
        char *label = lc_wizard_label(kind, i);
        if (!label) return -1;
        int hit = strcmp(label, want) == 0;
        lc_string_free(label);
        if (hit) return (long)i;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <library dir> <disc image>\n", argv[0]);
        return 2;
    }
    const char *library_dir = argv[1];
    const char *disc = argv[2];

    printf("== the wizard, through C\n");
    LcWizard *w = lc_wizard_new();

    long dos = label_index(LC_LABEL_FAMILY, "DOS");
    check("the family picker offers DOS", dos >= 0, NULL);
    lc_wizard_open_new(w, (size_t)dos);
    check("the form is open", lc_wizard_is_open(w), NULL);
    check_str("as \"New machine\"", lc_wizard_title(w), "New machine");

    /* The whole point of a shared form: these are the same answers the
     * two GUIs get, because it is the same code. */
    check("DOS opens on 64 MB", lc_wizard_ram_mb(w) == 64, NULL);
    check("...which is the family's default", lc_wizard_ram_is_default(w), NULL);

    char *cpu = lc_wizard_label(LC_LABEL_CPU_SPEED, lc_wizard_cpu_speed(w));
    check("a period processor, not full speed", cpu && strncmp(cpu, "486DX2-66", 9) == 0, cpu);
    lc_string_free(cpu);

    char *accel = lc_wizard_label(LC_LABEL_ACCEL, lc_wizard_accel(w));
    check("emulated, because a throttle needs TCG", accel && strcmp(accel, "Emulation") == 0, accel);
    lc_string_free(accel);

    check("no network card", !lc_wizard_network(w), NULL);
    char *note = lc_wizard_network_note(w);
    check("...and it says so", note && strstr(note, "No network adapter") != NULL, note);
    lc_string_free(note);

    /* The memory range is per family and a value outside it is clamped
     * here rather than refused at save time. */
    uint32_t min = 0, max = 0;
    lc_wizard_ram_range(w, &min, &max);
    lc_wizard_choose_ram_mb(w, max + 4096);
    check("memory is clamped to the family's ceiling", lc_wizard_ram_mb(w) == max, NULL);
    lc_wizard_reset_ram(w);
    check("...and \"Default\" puts it back", lc_wizard_ram_is_default(w), NULL);

    lc_wizard_set(w, "name", "capi dos");
    lc_wizard_set_flag(w, "existing_disk", true);
    lc_wizard_set(w, "disk_path", "/dev/null");
    check("submit", lc_wizard_submit(w, library_dir), NULL);

    char *saved = lc_wizard_saved_path(w);
    check("...wrote a bundle", saved && strlen(saved) > 0, saved);
    char bundle[4096];
    snprintf(bundle, sizeof bundle, "%s", saved ? saved : "");
    lc_string_free(saved);
    lc_wizard_free(w);

    printf("== the library\n");
    LcMachines *m = lc_machines_new();
    lc_machines_refresh(m);
    size_t found = (size_t)-1;
    for (size_t i = 0; i < lc_machines_count(m); i++) {
        char *name = lc_machines_name(m, i);
        if (name && strcmp(name, "capi dos") == 0) found = i;
        lc_string_free(name);
    }
    check("the machine is in the library", found != (size_t)-1, NULL);
    if (found != (size_t)-1) {
        check_str("its family", lc_machines_family(m, found), "DOS");
        check_str("its shader", lc_machines_shader_label(m, found), "(default)");
        check("nothing is running", !lc_machines_is_running(m, found), NULL);
    }

    printf("== the disc shelf\n");
    char *shelf_path = lc_machines_disc_library_path(m);
    LcShelf *s = lc_shelf_new();
    lc_shelf_open_for(s, bundle, shelf_path);
    check("the shelf opened for a machine", lc_shelf_for_machine(s), NULL);
    check_str("with an empty tray", lc_shelf_boot_label(s), "(empty tray)");
    size_t before = lc_shelf_count(s);
    lc_shelf_add(s, disc);
    lc_shelf_flush(s);
    check("a disc went on", lc_shelf_count(s) == before + 1, NULL);
    check("...and the shelf was written", lc_shelf_take_saved(s), NULL);
    check("...only once", !lc_shelf_take_saved(s), NULL);
    check("the new disc boots nothing yet", !lc_shelf_is_boot(s, lc_shelf_count(s) - 1), NULL);
    check("make it the boot disc", lc_shelf_set_boot(s, lc_shelf_count(s) - 1), NULL);
    check("...it is", lc_shelf_is_boot(s, lc_shelf_count(s) - 1), NULL);
    lc_shelf_set_label(s, lc_shelf_count(s) - 1, "a renamed disc");
    lc_shelf_flush(s);
    check_str("a label is editable", lc_shelf_label(s, lc_shelf_count(s) - 1), "a renamed disc");
    lc_shelf_free(s);
    lc_string_free(shelf_path);

    printf("== snapshots on a machine that is not running\n");
    LcSnapshots *snaps = lc_snapshots_new();
    lc_snapshots_open_for(snaps, bundle, false);
    /* Not running, so this went at the disk with qemu-img rather than
     * through a monitor. An image with no snapshot table has none — that
     * is not an error, and a front end must not show one. */
    char *err = lc_snapshots_error(snaps);
    check("a machine with no snapshots is not an error", err && strlen(err) == 0, err);
    lc_string_free(err);
    check("...and lists nothing", lc_snapshots_count(snaps) == 0, NULL);
    check("...with no job in flight", !lc_snapshots_job_pending(snaps), NULL);
    /* A bundle that isn't there is: a failure has to arrive as a message,
     * which is the part worth checking across a C boundary. */
    lc_snapshots_open_for(snaps, "/nonexistent/machine.toml", false);
    err = lc_snapshots_error(snaps);
    check("a bundle that isn't there reports why", err && strlen(err) > 0, err);
    lc_string_free(err);
    lc_snapshots_free(snaps);

    printf("== the shader profile editor\n");
    LcEditor *e = lc_editor_new();
    lc_editor_new_profile(e);
    check("a new profile has no parameters yet", lc_editor_param_count(e) == 0, NULL);
    check("...and nothing to render", !lc_editor_renderable(e), NULL);
    check("saving it without a name is refused", !lc_editor_save(e, NULL), NULL);
    char *save_err = lc_editor_error(e);
    check("...and says so", save_err && strcmp(save_err, "a name is required") == 0, save_err);
    lc_string_free(save_err);
    lc_editor_free(e);

    lc_machines_free(m);
    printf("\ncapi smoke: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
