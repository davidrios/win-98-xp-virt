# Track: M10 — the native Win98 display driver (doc 19, ADR-012)

The handoff for a session that gives Win98/Me the driver XP already has:
first the split of the XP driver into an OS-independent core plus a thin
per-OS layer, then the 9x layer on top of that core. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then doc 19, then doc 15 (which is the core's actual
specification — every behaviour the core has to keep is described there).

Branch `track/m10-win98-driver`, worktree
`.claude/worktrees/m10-win98-driver` (opened 2026-09-06, off `main` at
`8a0cfce`; submodules initialised, nothing built there yet).

## Scope and files (this track owns them)

- The guest driver source: `guest-tools/src/d3dptvid/` and
  `guest-tools/build-driver.sh`. **This is the M7 track's tree**; M7's
  work is all on `main` (its branch was fully merged), so there is no
  live conflict, but any M7 session that reopens must rebase on this
  split rather than edit around it.
- The 9x half once it exists: the 16-bit display driver, its VxD, its INF
  and its installer, plus whatever the DDK-header vendoring needs.
- `guest-tools/src/setup.c` — the display-driver component for the 98/Me
  role — and `tools/setup-guest-test.sh`'s Win98 expectations.
- Tests: a `tools/win98-driver-test.sh` mirroring `tools/xp-driver-test.sh`,
  and the Win98 side of whatever guest checks land in `scripts/test.sh`.
- Docs: `docs/19-win9x-display-driver.md`, this file, ADR-012 in doc 10,
  the M10 row of the state table and the M10 line of "Next steps" in
  `docs/00-status.md`.
- Shared with other tracks (rebase first, edit minimally, say which track
  in the commit): `d3dpt/d3dpt_fb.h` and `d3dpt/hw/d3dpt_vga.c` (M7's —
  the adapter should need *no* change for 9x; if it does, that is a
  finding worth writing down), `d3dpt/d3dpt_proto.h` and `d3dpt/exec/`
  (M4/M7's — the 9x driver speaks the existing protocol, a bump would
  mean 9x needs something XP does not), `guest-tools/build-wrappers.sh`,
  `scripts/test.sh`, `launcher/` (the Win98 reference machine's `-vga`),
  `CLAUDE.md`.

## State (2026-09-06)

Track opened, nothing built yet. What it starts from:

- XP's driver is complete through the DX8 DDI and is the thing being
  generalised: miniport + display driver, DirectDraw DDI, Direct3D DDI
  (DX3 execute buffers through DX8 with hardware T&L and vs/ps 1.x),
  protocol v9, all on `main`. Doc 15 and `docs/tracks/m7-display-driver.md`
  have the detail and the hard-won rules (dxg's caps rules, the flip
  role-swap, untracked GDI writes, the cursor register set).
- Win98 today: `-vga cirrus` with the inbox driver, 3D through the
  qemu-3dfx Glide wrappers and WineD3D DLLs per game folder. That path
  stays and is the control to measure against.
- `~/vms/win98.qcow2` is the guest image (ACPI install, TCG only — under
  KVM Explorer dies at startup). `tools/setup-guest-test.sh <image> win98`
  is the existing headless Win98 harness to build on.

## Next steps, in order

1. **Step 0 — establish the 9x driver model** (doc 19, "What 9x does
   differently"). Read `vmdisp9x` for the `.drv` + VxD split, how a
   DirectDraw HAL is published from a 16-bit driver, whether the DDHAL /
   D3DHAL structures match NT's field for field where our core reads
   them, and how far the DDI goes on 9x (DX8 or DX7). Write the answers
   into doc 19 — this is the single biggest unknown in the track and
   every estimate below depends on it. Deliverable: doc 19's step-0
   section replaced by facts, and a "hello world" 9x display driver that
   brings up a mode on `d3dpt-vga` and nothing else.
2. **Step 1 — the split, XP unchanged.** Carve `core/` out of
   `d3dptdisp.c` per doc 19, thunk NT onto it, and prove it is a
   refactor: `scripts/test.sh all` green, `d3dpt-dp2-test` and `d3d7test`
   against the same golden BMP, `shtest` / `cktest` / `ebtest` / `dxttest`
   at the same case counts, `d3dgame8` still matching the native oracle,
   Moto Racer and Vice City still drawing (`tools/xp-driver-test.sh`,
   `tools/xp-motoracer.sh`, `tools/xp-vicecity.sh`). Land this on its own
   — a 9x bug on top of an unproven refactor is two bugs wearing one
   coat.
3. **Step 2 — the 9x framebuffer driver** (98's M7a): modes from the
   miniport's table, the desktop on the adapter, `d3dptvid: adapter
   found` from the device, no copy inside QEMU. Installed by INF from the
   guest-tools ISO; `SETUP.EXE` grows the component and
   `tools/setup-guest-test.sh win98`'s "never offered" check inverts.
4. **Step 3 — the DirectDraw DDI on 9x** (98's M7b): the VRAM heap, the
   flip chain against the frame counter, `DdMapMemory`'s equivalent,
   8 bpp palettized modes. `DDTEST.EXE` and `CDTEST`-style guest probes
   run on 98 as they do on XP.
5. **Step 4 — the Direct3D DDI on 9x** (98's M7c): the core's DP2 walker
   under the 9x HAL. `D3D7TEST.EXE`'s frame must match
   `d3dpt-dp2-test`'s BMP exactly, the same oracle XP is held to. Then
   `EBTEST` (the DX3 path, which is most of the 98 matrix), `CKTEST`,
   `DXTTEST`, and `SHTEST`/DX8 only if step 0 says 9x's runtime goes
   there.
6. **Step 5 — the titles.** The doc 04 Win98 acceptance matrix through
   the driver, against the same titles on the Glide/WineD3D stack: which
   is faster, which is correct, and what the launcher should default to.

## Build / test loop

```sh
guest-tools/build-driver.sh                 # the driver + DRIVER\ ISO (both OSes once step 1 lands)
guest-tools/build-wrappers.sh               # the big guest-tools ISO
scripts/test.sh                             # host stage (~30 s); `all` adds the guest stage
tools/setup-guest-test.sh ~/vms/win98.qcow2 win98    # SETUP in a real 98, headless
tools/xp-driver-test.sh ~/vms/winxp-m7c.qcow2 d3d7   # XP's regression oracle across the split
```

This worktree has its own submodules but no `build/`. It needs
`scripts/build.sh` once (~15 min for QEMU from scratch on the Linux box);
`build/dxvk` may be symlinked from the main checkout rather than rebuilt.

Notes that cost time if forgotten (CLAUDE.md has them, they bite here):

- **Win98 runs under TCG, not KVM** — under KVM the image loses Explorer
  at startup and there is no way to drive the guest.
- **End every scripted Win98 run with a Start-menu shutdown**, never a
  kill: a killed VM leaves the FAT dirty and the next boot runs ScanDisk.
- Win98 must be an ACPI install (`SETUP /p j`) or PCI hot-adds are never
  seen — which is exactly how the adapter arrives.
- Never write the user's own images: run on overlays or on the m10 copies.
- Kernel-mode debugging is the device's DEBUG register into the QEMU log,
  never a debugger.
