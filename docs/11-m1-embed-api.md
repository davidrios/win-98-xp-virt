# 11. M1 design: `libqemu_embed` — QEMU in-process

Derived from a source survey of QEMU v9.2.4 (patched tree). File:line refs
are to `qemu/` as prepared.

## Shape

One shared library per target, `libqemu-embed-i386.{so,dylib,dll}`, built by
QEMU's own meson from the existing per-target static lib
(`meson.build` ~4227: `lib = static_library('qemu-' + target, ...)` already
excludes `system/main.c`, so there is no `main()` to fight) plus our shim
`embed/libqemu_embed.c`. The player links it and drives it through
`embed/libqemu_embed.h` (bindgen). Our sources live in `/embed/` and are
overlaid into `qemu/embed/` by `prepare-qemu.sh`, like the 3dfx devices;
`patches/qemu/10-embed-api.patch` only touches `meson.build`.

## What needs no QEMU changes (the big win)

- **Lifecycle.** `qemu_init(argc, argv)` → `qemu_main_loop()` →
  `qemu_cleanup()` (`include/sysemu/sysemu.h:98-100`), called by us on one
  caller-created thread. `qemu_init` takes the BQL on the calling thread
  (`system/runstate.c:864`, thread-local ownership `system/cpus.c:515`), so
  init and the main loop **must be on the same thread**. We always pass
  `-S` so the guest is paused when init returns (otherwise `qmp_cont` runs
  before displays exist, `vl.c:2751`), hook up, then `vm_start()`.
- **Display.** Pass `-display none` and, after init (BQL held), register a
  `DisplayChangeListener` on `qemu_console_lookup_default()`
  (`ui/console.c:694`). Callbacks: `dpy_gfx_switch` (new surface; the old
  one is freed on return — never retain it, `ui/console.c:853`),
  `dpy_gfx_update` (clamped dirty rect), `dpy_refresh` (we call
  `graphic_hw_update()` — this is the pull that makes the VGA device render;
  the GUI timer only exists if some listener has `dpy_refresh`,
  `ui/console.c:108-127`), `dpy_gfx_check_format` (accept `x8r8g8b8` only in
  v1 → QEMU shadows 8/15/16/24bpp into 32bpp for us), cursor/mouse-set.
  All fire on the main-loop thread under BQL. Registration synchronously
  fires switch+update+cursor before returning. Zero-copy is real for 32bpp
  modes (surface points into VRAM, `hw/display/vga.c:1637`), shadowed
  otherwise.
- **Input.** `qemu_input_event_send_key_qcode`, `qemu_input_queue_rel/abs/btn`
  + `qemu_input_event_sync` (`include/ui/input.h`). Must run under BQL and
  is dropped while paused. We enqueue from any thread and drain in a
  bottom-half (`aio_bh_schedule_oneshot(qemu_get_aio_context(), …)`), one
  sync per batch. Scancode→QKeyCode via the exported
  `qemu_input_map_atset1_to_qcode` table. `qemu_input_is_absolute()` +
  mouse-mode notifier tell the player whether the guest wants tablet or
  PS/2 semantics.
- **VM control.** `qemu_system_{vmstop,reset,powerdown,shutdown}_request()`
  are async and thread-safe; `vm_start()` needs BQL → bottom-half.
- **QMP.** `socketpair(AF_UNIX)` in the player, pass one end as
  `-chardev socket,id=qmp0,fd=N -mon chardev=qmp0,mode=control`. Full QMP
  incl. events, monitor runs on its own iothread. No filesystem path, no
  network — good enough for "in-memory". (True pipe: `qemu_chr_open_fd()`
  on a `TYPE_CHARDEV_PIPE`, ~15 lines, if ever needed.)

## What needs patches

1. `10-embed-api.patch` — meson: `shared_library('qemu-embed-<target>',
   files('embed/libqemu_embed.c'), objects: lib.extract_all_objects(...),
   dependencies: arch_deps + [sdl], link_args…)` next to the executable.
   Objects are linked directly (not via archive) so `type_init`
   constructors survive. Requires `-fPIC` (configure-qemu.sh adds it).
2. `20-embed-audio.patch` (next step, not in the first cut) —
   `audio/embedaudio.c` modeled on `noaudio.c` with `dbusaudio.c`'s
   `get_buffer_out`/`put_buffer_out` so the mixer clips straight into our
   ring; plus the ~6-line QAPI enum/union entry (`qapi/audio.json`) and
   `audio_template.h` CASE. Launch with
   `-audiodev embed,id=snd0,out.frequency=48000,out.channels=2,out.format=s16`
   — `fixed_settings` gives one voice with exactly those settings.

## Hazards recorded

- `qemu_init` errors are `error_fatal` → `exit(1)`: validate config before.
- `os_setup_signal_handling()` installs SIGINT/SIGHUP/SIGTERM handlers
  process-wide (`os-posix.c:57`): save/restore around init in the player.
- `qemu_cleanup` is incomplete (`runstate.c:929` TODO): **one VM per process
  lifetime** — matches our launcher/player split (doc 02).
- qemu-3dfx's `graphic_hw_passthrough()` makes `graphic_hw_update` skip the
  device (`ui/console.c:147-152`) while 3D is active, and 3dfx/mesa render
  into QEMU's SDL2 window — **and refuse to activate without it**
  (`sdl_display_valid()` exits the process). So with `-display none` the 2D
  path works and any 3D title kills the VM until M3 replaces the SDL-window
  dependency (Spike A doc). The player must not advertise 3D before then.

## Embed API (v1)

See `embed/libqemu_embed.h`. Thread contract: `*_new/run/destroy` on one
thread; display callbacks on that thread (BQL held, must not block);
everything else callable from any thread.

## Player side (M1 steps)

1. `qemu-embed-sys` crate: bindgen over the header, links the shared lib
   from `build/qemu/`.
2. Player spawns the QEMU thread, gets frames via callbacks into a
   triple-buffered `Vec<u32>` staging (copy dirty rects under the callback,
   publish on `on_refresh_done`), presents through the existing wgpu path.
3. Keyboard/mouse from winit → `qemu_embed_key/mouse_*` + `flush`.
4. FreeDOS boot → Win98 boot. Then audio patch, QMP, librashader.
