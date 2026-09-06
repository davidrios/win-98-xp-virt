# 10. Decision records

Short ADR-style log. Newest last. Docs elsewhere reflect the current state;
this file preserves the *why*.

## ADR-001: QEMU as the base (2026-08-31)

Only open-source option covering Linux/Windows/macOS incl. Apple Silicon with
a working guest-3D path for both Win98 and XP (qemu-3dfx). VMware rejected
(closed source, no ARM mac). VirtualBox rejected (3D for pre-Win7 guests
removed in 6.1+). 86Box out of scope (authentic-hardware emulation already
exists; we don't duplicate it).

## ADR-002: QEMU runs in-process with the display (2026-08-31)

Gaming latency requirement: framebuffer, input, and audio must not cross a
process boundary. UTM-style embed patches. Consequence: one VM per hosting
process; GPL-2.0 for anything linking QEMU.

## ADR-003: Primary frontend is a libretro core; custom app demoted to companion (2026-08-31)

A RetroArch core replaces most of the planned custom display pipeline and
player: native slang CRT shaders, mature frame pacing, input capture, audio,
disk-control API for CD swapping, cross-platform packaging, existing
user base. Precedent: DOSBox Pure (Win98-in-RetroArch works and has demand).
The `libqemu_embed.h` layer is frontend-agnostic, so a standalone player
remains possible later at low cost; the **companion launcher** (machine
creation, snapshots, disc shelf, guest-tools media) covers what RetroArch's
content model does badly. Accepted risks: qemu-3dfx GL inside libretro
hw-render contexts on macOS (spike scheduled in M0), large save states,
living with libretro API constraints.

## ADR-004: Rust where possible (2026-08-31)

All original code is Rust unless a hard boundary forces C:

| Component | Language | Why |
|---|---|---|
| libretro core shell (cdylib) | **Rust** | thin FFI over small API |
| embed-API bindings | **Rust** (bindgen) | consumer side |
| companion launcher | **Rust** | greenfield GUI |
| CD disc-model crate + format parsers ("libdisc") | **Rust** | greenfield, parser-heavy = Rust sweet spot; QEMU upstream now accepts Rust (experimental since 9.2), so upstreaming is plausible |
| MMC exerciser, tooling, CI | **Rust** | greenfield |
| QEMU patches: embed API, ATAPI device glue | C | inside QEMU; thin shims calling libdisc |
| qemu-3dfx host patches | C | third-party, version-coupled |
| Guest-side wrappers/drivers | C (era toolchains) | Rust cannot target Win9x/XP |

Consequence for doc 05: disc model is implemented in Rust; libmirage becomes
a behavioral reference (readable GPL source, format documentation), not a
linked dependency — this also drops its glib dependency problem.

## ADR-005: Standalone player is the frontend; RetroArch/libretro dropped (2026-09-02)

Supersedes ADR-003. After hands-on time with RetroArch the user judged it too
buggy to build on ("the thing is so full of bugs it's not even worth it").
The frontend returns to the original design: a **standalone Rust player**
(winit + wgpu + librashader + egui + cpal) with in-process QEMU (ADR-002
unchanged), plus the **launcher** for machine management — the two-process
model from the first architecture draft. Consequences:

- We own the whole presentation pipeline again (doc 03 fully in scope): CRT
  shaders via librashader's wgpu runtime (Metal on macOS), frame pacing,
  input grab, audio. More work than the core route, but no dependence on
  RetroArch's quality or API constraints.
- qemu-3dfx integration target becomes "guest GL output → wgpu texture"
  (Spike A retargeted: docs/spikes/spike-a-macos.md) instead of a libretro
  hw-render context. No libretro hw-render risk on macOS anymore; the
  GL↔Metal interop question remains and is the spike.
- Spike B (libretro crate choice) is void. The M0 libretro core validated in
  RetroArch is deleted; its test pattern lives on in `player/`.
- The embed API stays frontend-agnostic on principle; a libretro shell could
  be re-added by a third party, we won't maintain one.

## ADR-006: Direct3D 8/9 for XP through our own paravirtual device (2026-09-03)

**Decision.** Direct3D 8/9 acceleration for the XP guest is a paravirtual
Direct3D device of our own: thin guest `d3d9.dll`/`d3d8.dll` that serialize
the API into a shared-memory command stream, and a host-side executor that
runs the same D3D9 semantics natively (DXVK's d3d9 over Vulkan, MoltenVK on
macOS). Design in doc 14. Guest-side WineD3D (JHRobotics' wine9x build on
the guest-tools ISO) stays as the fallback and as the only path for
DirectDraw / Direct3D ≤7 until the device grows a ddraw layer.

**Why now.** Two days with WineD3D-in-guest on XP (FIFA 2000, doc 00):

- Every fix so far was in a 2015 fork with no upstream: 24-bit desktop
  modes (`patches/wine9x/01`), front-buffer presentation on our FBO stand-in
  (`embed/mglcntx_embed.c`), and the open ones (palettized/dynamic texture
  corruption, per-flush flicker, DirectInput focus after the mode switch).
  Each is diagnosable, none is the last.
- The structural cost is unfixable there: WineD3D does all state tracking
  and D3D→GL translation *inside the emulated x86 guest*, at TCG speed,
  and then ships GL calls through the FIFO one by one. A paravirtual device
  moves that translation to native host code; the guest only serializes.
  On Apple Silicon that is the difference between "works" and "plays".
- Doc 04 already listed "a community D3D9 paravirt device, if it
  materializes" as the adoption target. None has (VirtualBox's WDDM/DX
  path is Vista+, VMware's SVGA3D is closed and Win7+). We build it.

**Alternatives rejected.** *Proton*: a different product (Windows games on a
Linux host, no Windows guest, no macOS) — its components, not its shape, are
relevant. *DXVK in the guest*: needs a Vulkan driver in XP; none exists.
*Keep fixing WineD3D 1.7.55*: see above; it remains the fallback.

**Licensing.** Wine is LGPL-2.1-or-later: copying Direct3D behaviour *and
code* from current Wine (d3d9/d3d8 COM plumbing, d3d8→d3d9 mapping, format
tables, tests) into the guest DLLs and the host executor is allowed; those
components carry the LGPL, ship with source, keep their headers. DXVK is
zlib. d3d8to9 is BSD-2. QEMU is GPL-2.0 and qemu-3dfx GPL-2.0; all of these
combine (LGPL/zlib/BSD code inside a GPL-2.0 program is fine). Nothing from
Microsoft's SDK ships: headers come from mingw-w64 (public domain / ZPL).

**Consequences.** New milestone in doc 08 (M4 becomes the device). The
qemu-3dfx FIFO/MMIO model, the FXPTL/MAPMEM guest driver, the embed frame
path (IOSurface / dma-buf) and the player's 3D layer are reused as-is; the
new pieces are the guest DLLs, a `d3dpt` device in QEMU, and a host executor
linking DXVK. C++ enters the host side (DXVK) behind a C shim; guest DLLs
are C. Apple Silicon depends on DXVK-over-MoltenVK being good enough for
D3D9-era feature levels: spike first (doc 14 P0), fallback is a host-side
WineD3D or a wgpu backend later. The x87 work (patch 06, doc 13) is
unaffected: games still set PC=24 through our d3d9.dll's CreateDevice.

## ADR-007: The host executor is DXVK on every platform; macOS runs it over KosmicKrisp (2026-09-03)

**Decision.** The paravirtual Direct3D device's host executor (ADR-006,
doc 14) is DXVK's d3d9, built natively as a library next to QEMU, on Linux
and macOS alike. On macOS the Vulkan implementation under it is **Mesa's
KosmicKrisp** (LunarG's Vulkan-on-Metal-4 driver, Vulkan 1.4 conformant on
Apple Silicon, prebuilt in the LunarG macOS SDK), which requires **macOS 26**
— the Air gets upgraded. MoltenVK is not a supported configuration.
DXVK is carried as a submodule (`third_party/dxvk`) plus a patch queue
(`patches/dxvk/`), the same discipline as the QEMU fork.

**Why.** Spike C (`docs/spikes/spike-c-dxvk-native-macos.md`): DXVK
master refuses MoltenVK 1.4.2 outright — five hard-required features are
missing (geometry shaders, cull distance, depth-clip-enable,
robustBufferAccess2, nullDescriptor), and the two robustness ones are
unimplementable without shader-side bounds checks Metal lacks (MoltenVK
issue open since 2025-02). KosmicKrisp advertises all of them except
geometry shaders, which d3d9 never uses: a one-line "required → optional"
patch. DXVK's core compiles on macOS; only its Linux-only Windows shim
needs ~30 lines.

**Alternatives rejected.** *Implement the features in MoltenVK*: harder than
patching DXVK for the same result. *DXVK-on-MoltenVK with a dummy-resource
patch queue* (the Gcenx DXVK-macOS pattern): viable as a bridge but fights
DXVK's descriptor-heap design on every rebase; not pursued since the OS
upgrade is accepted. *A native D3D9-on-Metal executor inspired by DXVK*:
a rewrite of ~40 k lines plus years of fidelity work, and a second executor
to keep bug-for-bug equal with Linux; last resort only. *wgpu translator*:
same magnitude.

**Consequences.** Minimum macOS for the player's Direct3D path is 26
(Tahoe); GL pass-through and everything else keep working on 15. The
player links DXVK through a C shim; a window-less WSI backend replaces
SDL2 (the device renders off-screen into the existing IOSurface / dma-buf
frame path). KosmicKrisp is young: Metal 4 workarounds for M1/M2 on macOS
26 live in the driver (fixed in 27); the rig goldens (`reference/d3d`)
are the acceptance test for both drivers. **Verified 2026-09-03** on the
Air (macOS 26.6.2, SDK 1.4.357.1): one more required feature had to be made
optional (`fillModeNonSolid`, patch 05), then the reference frame matches
the rig as closely as on RADV (spike C, "Verified on KosmicKrisp").

## ADR-008: A real guest display driver is the long-term shape; staged after the DLL device (2026-09-04)

**Decision.** The paravirtual Direct3D device keeps ADR-006's shape today
(guest `d3d9.dll` / `d3d8.dll` serializers → `d3dpt` device → DXVK
executor), and we commit to growing a **real Windows display driver** on
top of the same transport and executor, in stages, as milestone M7 (doc
08). The driver replaces the DLL copies per game folder, replaces the
emulated Cirrus as the guest's display adapter, and is the only road to
Vista/7/10 acceleration should we ever want it. Nothing below the guest
side changes: the shared window, the doorbell, the record protocol
(`d3dpt/d3dpt_proto.h`) and `libd3dpt_exec` are the driver's back end as
they are the DLL's.

**Why.** After P1/P2 (2026-09-03/04) the DLL device works and matches the
rig, so the question is no longer "can the host run D3D9 for the guest"
but "how do games reach it". The DLL answer has structural limits:
- **Per-game installs.** A D3D9.DLL must sit next to every EXE (system-wide
  replacement fights Windows File Protection on XP), games that resolve
  `system32\d3d9.dll` by path, load through DirectDraw 7 in the same
  process, or check DirectX versions do not see it. On the rig the same
  folder-level fiddling already bit the reference workloads.
- **We re-implement the runtime.** State shadowing, managed pool, lost
  device, state blocks, software vertex processing: every semantic of
  Microsoft's d3d9.dll has to be re-done in our DLL. With a display driver
  Microsoft's runtime stays in the guest and talks to us through the
  **Direct3D DDI**, which on 2000/XP is *already a serialized command
  stream*: `D3dDrawPrimitives2` hands the driver DP2 token buffers (render
  states, texture stage states, draws, shader creation and constants —
  `d3dhal.h`, 32 opcodes), surfaces come through the DirectDraw DDI
  (`DdCreateSurface`, `DdLock`, `DdBlt`), GDI through `winddi.h`. That is
  our record stream with Microsoft doing the validation.
- **The desktop becomes ours.** A display driver owns the framebuffer and
  the mode list: any resolution and refresh the CRT shader wants (M2's
  mode table and pixel aspect), no Cirrus limits (XP has no driver for
  QEMU's standard VGA), DirectDraw and Direct3D 7 for free, a zero-copy
  2D path through the shared window. This is what VMware (SVGA II +
  SVGA3D) and VirtualBox (their XPDM/WDDM additions) do; both prove the
  shape and both show the cost.

**Alternatives kept in their place.** *The DLL device* stays: it is the
Win98 path regardless (9x has no comparable driver model worth targeting),
it is the debugging harness for the executor, and it ships now. *WineD3D
in the guest* stays the DX7 fallback until the driver's DirectDraw DDI
exists. *Vista/7/10 (WDDM: kernel miniport under dxgkrnl plus user-mode
D3D9/D3D10/D3D11 UMDs)* is explicitly **not** in scope for v1; the
decision only keeps the door open by choosing the transport that leads
there. A D3D11 UMD over our device is the size of VBoxDX, a multi-year
team effort.

**Staged plan (M7).**
1. **M7a — framebuffer driver (2D).** A video miniport (`video.h`, loaded by
   videoprt.sys) that exposes our shared window as the frame buffer and a
   mode list we control, plus a display driver DLL (`winddi.h`) that does
   GDI in software on that buffer (the `framebuf`/`vga` shape; ReactOS
   builds the same kind with GCC, ~3 k lines). QEMU side: a paravirtual
   framebuffer register set on the `d3dpt` device (mode, pitch, dirty
   rectangles, vsync) and the player showing it through the existing frame
   path. Exit: XP desktop at 1024×768@85 on the driver, zero-copy, no
   Cirrus, ScanDisk-clean shutdowns, a mode table the CRT presets pick
   from. This step pays for itself (M2) and teaches the kernel-side
   workflow: BSOD debugging over serial with WinDbg or the ReactOS tools,
   a `.inf` install, driver signing not required on XP.
2. **M7b — DirectDraw DDI.** `DdCreateSurface`/`DdLock`/`DdBlt`/`DdFlip` on
   the same driver → surfaces in the device's handle table, blits by the
   executor (DXVK `StretchRect`/`UpdateSurface`), overlays refused. Exit:
   DirectX 5–7 titles (FIFA 2000, doc 00) without WineD3D.
3. **M7c — Direct3D DDI.** `D3dContextCreate`, `D3dDrawPrimitives2`: the DP2
   token consumer that translates into our records (mostly 1:1), texture
   and shader DDIs, caps from the executor. Exit: the doc 04 matrix
   through Microsoft's own d3d8/d3d9 with no DLL in the game folder; the
   DLL device becomes the 9x path only.
4. **M7d — later, if ever:** WDDM for Vista+ (out of scope for v1).

**Toolchain and licensing.** mingw-w64 ships the DDI headers (`ddk/winddi.h`,
`ddk/video.h`, `d3dhal.h`) under its permissive terms; kernel-mode display
DLLs and miniports are plain PE files with no CRT, buildable with the
same cross toolchain as the wrappers (the build script's msvcrt/ISA
checks extend to them). ReactOS (GPL-2.0) is the reference for building
such drivers with GCC and for DDI behaviour; Wine has no display-driver
side. Nothing from Microsoft's DDK ships.

**Consequences.** Doc 08 gains M7 after M4; doc 14 P5's "proper PnP driver
instead of MAPMEM" becomes M7a. ADR-006 stands. The record protocol must
stay driver-neutral: no guest-DLL-only assumptions (handles are guest
chosen, resources are host objects, nothing in it knows about COM). The
executor grows the DDI-shaped operations M7b/c need (blits between
surfaces, DP2 semantics such as per-primitive vertex buffers) when those
stages start, not before.

## ADR-009: The launcher (and `shader-chain`) are GPL-2.0-or-later (2026-09-05)

**Decision.** `launcher` and `shader-chain` declare `GPL-2.0-or-later`
instead of the workspace's `GPL-2.0-only`. Everything that links QEMU —
`player`, `qemu-embed` — and `libdisc`, which is compiled into QEMU
itself, stay `GPL-2.0-only`.

**Why.** The launcher's dependency tree contains crates licensed
**Apache-2.0 with no alternative**, and Apache-2.0 is incompatible with
GPLv2 (the patent-termination clause) while being fine with GPLv3. It is
not one stray crate and it did not arrive with the preset downloader:
egui/eframe brings `ab_glyph`, `ab_glyph_rasterizer`, `accesskit_winit`
and `glutin`; winit brings `dpi` (`Apache-2.0 AND MIT` — an *and*); the
download added `ring` (`Apache-2.0 AND ISC`) under `ureq`'s rustls. The
conflict was there from the day the launcher was an egui app; the
downloader is what made someone look. `GPL-2.0-or-later` resolves it,
because a recipient may take the combined binary under v3.

The launcher can do this and the player cannot: **the launcher links no
QEMU code.** It writes bundles, spawns `player` as a separate process
and talks to it over a QMP socket (doc 07 — the launcher is optional by
design). The `GPL-2.0-only` pin exists for the in-process QEMU library,
which the launcher never touches. `shader-chain` moves with it because
it is linked into *both* binaries: relicensing the launcher would be
worth nothing if a v2-only crate rode along into the same binary, and
"or later" still combines into the player's v2-only whole exactly as
before.

**What this does not settle.** The *player* has the same Apache-2.0
exposure — `ab_glyph` (egui overlay), `cpal` (audio), `codespan-
reporting` (via naga) — and it links QEMU, so "or later" is not
available to it. That is a real open question for anyone distributing
player binaries, not something this ADR fixes; it needs its own pass
over which of those crates are replaceable. There is also no `COPYING`
or per-crate licence file in the tree yet, only the Cargo metadata and
the README section.

**Alternatives rejected.** Moving the preset download out of process
(`curl`/`git`) removes one instance and leaves the egui/winit ones
standing. `aws-lc-rs` in place of `ring` brings the OpenSSL licence in;
`native-tls` on Linux *is* OpenSSL 3, also Apache-2.0. Shipping the
presets as a packaging payload was rejected on its merits anyway: the
collection is 80 MB unpacked and is upstream's to update.

## ADR-010: The player ships as a binary despite the GPLv2/Apache-2.0 conflict (2026-09-05)

**Decision.** The player's dependency tree contains Apache-2.0-only code
and the player links GPL-2.0-only QEMU. The two are, on the FSF's reading,
incompatible in one binary. **We ship player binaries anyway** (M6 step 6:
signed macOS .app, Windows installer, Linux Flatpak), state the position
in the README and in `THIRD-PARTY-NOTICES.md`, and distribute the complete
source and build scripts as we always have. This ADR records why that is a
considered position rather than an oversight.

**The conflict, precisely.** Apache-2.0's patent-termination clause is an
additional restriction GPLv2 does not permit (it is fine with GPLv3, which
is why ADR-009 could move the launcher). The player cannot follow the
launcher, for two independent reasons:

1. *QEMU pins it to v2.* QEMU's LICENSE is encouraging at first read —
   files with no header are GPLv2-**or-later**, and it says v2-only
   contributions are accepted only for `bsd-user/`, `linux-user/`,
   `hw/vfio/`, `hw/xen/xen_pt*`, none of which we link. Scanning the
   headers of what an i386 softmmu build actually compiles: 411 v2-or-later,
   605 with no licence header, and **35 genuinely v2-only** — among them
   `util/bitmap.c` (taken from Linux), `util/qemu-sockets.c`,
   `migration/migration.h`, `system/runstate-action.c`, and
   `hw/audio/ac97.c`, which is the XP machine's own sound card. Re-check
   with `tools/gpl-scan.py` after a QEMU bump; one v2-only file is enough.
2. *The crates are not swappable.* `winit` and `cpal` could go (SDL2 is
   Zlib and covers both), but `codespan-reporting` comes with naga and
   `rspirv`/`spirv` with librashader-reflect, so being clean means dropping
   **wgpu and librashader** — ADR-005's whole stack, and the CRT chain is
   the product. Partial removal changes the legal position not at all, so
   it is not worth doing for licence reasons.

**Why shipping anyway is defensible.** Every GPLv2 program built on the
modern Rust GUI stack is in this position — `winit` alone settles it — so
this is a property of the ecosystem, not a careless dependency choice. The
obligation attaches to distributing the combined binary; the source we
distribute is complete, buildable and unencumbered, which is the thing GPL
enforcement actually exists to protect. The residual risk is a strict
distribution (Debian, Fedora legal) declining to package the player, and a
QEMU copyright holder objecting — remote, and neither is silenced by any
alternative short of the process split below.

**Rejected: `dlopen` instead of linking.** `qemu-embed/build.rs` already
links `libqemu-embed-<target>` as a shared library, and resolving symbols
later changes nothing: the FSF treats static and dynamic linking alike, and
our coupling is at the far end of the scale it describes — one address
space, C structs and function pointers both ways, display callbacks on
QEMU's own vCPU threads with the BQL held, a lock-free audio ring both
sides write. The contrary reading (dynamic linking creates no derivative
work) is held in good faith by competent people and settled nowhere; a
project whose stance is "everything open source" should not lean on it.

**Rejected for now, and kept on the table: QEMU in its own process.** This
is the one structurally clean answer — separate address spaces, a versioned
protocol, each side independently implementable, which is the FSF's own
"arm's length" description and the shape `d3dpt_proto.h` and
`cdshelf_proto.h` already have here. **ADR-002 put QEMU in-process for
latency, and that premise was measured rather than assumed**
(`tools/ipc-latency-spike.c`, on the x86-64 rig, 16 cores):

| | idle | all cores busy |
|---|---|---|
| frame notify round trip, hot loop | p50 8 µs, p99 11 µs | p50 11 µs, p99 15 µs |
| frame notify @60 Hz, cold receiver | p50 18 µs, p99 226 µs, max 459 µs | p50 17 µs, p99 35 µs, max 2.0 ms |
| buffer fd over `SCM_RIGHTS`, once per ring slot | 12–43 µs | 17–2800 µs |
| in-process callback (today) | 0.02 µs | 0.02 µs |

Against a 16.7 ms frame that is 0.1 % typical and ~1.4 % at p99, and with a
three-slot ring the VM side never blocks on a release. **Latency is not
what stops this.** What stops it today is the work: the VGA surface would
have to reach the frontend through shared memory (a patch so QEMU allocates
its `DisplaySurface` from our mapping, or a dirty-rect copy), macOS would
need IOSurface handles over a mach port rather than `SCM_RIGHTS`, and the
lifecycle rules, headless dumps and `tools/xp-game-test.sh` all assume one
process. The seam is good — `player/src/qemu_vm.rs` is already the only
module that touches the embed API — so this is a track, not a rewrite.
Re-run the spike on the M1 Air before committing to it; the answer could
differ there.

**Consequences.** `COPYING` (GPLv2) and `THIRD-PARTY-NOTICES.md` are now in
the tree, and the README says the position in plain language so a packager
meets it up front. If a distribution refuses the player on these grounds,
that is the signal to open the process-split track, with the numbers above
already in hand rather than re-derived under pressure.

### ADR-009 addendum: permissive dual-licensing considered and rejected (2026-09-05)

Dual `MIT OR Apache-2.0` for our own code — the Rust ecosystem norm, and a
way to make `libdisc`, the protocol headers and the guest-side programs
reusable outside a GPL project — was tried (25 SPDX headers flipped) and
**reverted the same day. Everything of ours stays GPL.** Three reasons, in
the order they decided it:

- **It fixes nothing.** The player binary is GPLv2 because QEMU is, so the
  Apache-2.0 crate conflict of ADR-010 is untouched by our own terms.
- **The player side is GPL by design.** Its source is barely usable without
  linking a GPL library, so a permissive label there would be true and
  meaningless, and would invite the misreading that the binary is permissive.
- **The piece with the clearest reuse value is transient.** The guest D3D
  serializer DLLs are the obvious thing another project would want, and
  ADR-008 already demotes them: once M7c lands, the XP path is the display
  driver's own DDI and the DLLs are the 9x fallback.

Weighed against that, copyleft keeps derivatives of the novel work (the
paravirtual D3D protocol, the XP display driver, the CD-ROM model) open, and
the GPL projects most likely to reuse any of it — 86Box, DOSBox-X — are
GPL-compatible already. Permissive release is a one-way door; this side of it
stays open.

"Everything GPL" includes ADR-009's `GPL-2.0-or-later` for `launcher` and
`shader-chain`, which is what keeps the launcher's Apache-2.0 dependencies
permissible; only the QEMU-linking crates need `GPL-2.0-only`.

## ADR-011: the product is 2ksbox; the application ID is `com._2ksbox.Launcher` (2026-09-05, amended 2026-09-06)

**Decision.** The project's name is **2ksbox** (the user registered
`2ksbox.com`), and the application ID everything desktop-facing keys off
is **`com._2ksbox.Launcher`**. `win98-xp-virt` was always a working name;
when the decision was first written only the *packaged identity* had
moved (M6 step 6a). **Amended 2026-09-06:** the working name is gone
entirely — the repository is `github.com/davidrios/2ksbox`, the checkout
directory, the docs and the user's data directory all say 2ksbox.

**Why the underscore.** `com.2ksbox.Launcher` is not a legal application
ID: no segment of a D-Bus-style name may start with a digit, and
`flatpak build-init` refuses it outright — *"Name segment can't start
with 2"* (checked, not assumed; `appstreamcli validate` accepts the
underscore form). Escaping the leading digit is the established fix, the
same one that gives `7-zip.org` `org._7zip.…`.

**What carries which name.**

- **`2ksbox`** is the product: the installed commands (`2ksbox`,
  `2ksbox-player`), the resource directories (`share/2ksbox`,
  `lib/2ksbox`, `libexec/2ksbox`), the tarball, the window title.
  `launcher/src/paths.rs::NAME`.
- **`com._2ksbox.Launcher`** is the application: the desktop entry's
  filename, the icon's name, the Wayland `app_id` a compositor matches
  between the two, and — when 6b lands — the Flatpak and AppStream ID.
  `launcher/src/paths.rs::APP_ID`. Verified end to end: an installed
  launcher's window reports `app_id: com._2ksbox.Launcher`, exactly the
  basename of the installed `com._2ksbox.Launcher.desktop`.

**The data directory (the 2026-09-06 amendment).** The user's library —
machines, `discs.toml`, shader profiles, a downloaded preset collection —
is `~/.local/share/2ksbox`, and the runtime files are under
`$XDG_RUNTIME_DIR/2ksbox`. Moving a real user's library is the reason
this waited for a decision of its own rather than riding along with a
packaging change; it is done by `launcher/src/paths.rs::data_dir`, once,
the first time anything asks for the directory:

- a **rename inside the same parent**, so it is atomic — there is no
  window in which half a library exists in each place, which is what a
  copy-then-delete migration would have to defend against;
- **only when the new name does not exist**. Someone running two versions
  side by side has both directories; they are told which one is in use
  (a stderr line) and neither is touched, because merging two libraries
  behind the user's back is not a thing a rename should decide;
- **never fatal**. A failed move is a warning and an empty library, which
  the user can fix with one `mv`; refusing to start would leave them with
  no way to reach the UI that explains it.

The runtime directory is not migrated: what lives there belongs to a
running process, and a stale socket path outlives nothing.

## ADR-012: Win98 gets the same display driver as XP, over one shared core (2026-09-06)

**Decision.** Windows 98/Me gets a **native display driver for our
`d3dpt-vga` adapter**, the way XP already has one (ADR-008, doc 15) —
desktop modes from our table, a DirectDraw HAL, a Direct3D HAL feeding
the same `d3dpt` protocol and the same host executor. And before that 9x
driver is written, XP's driver is **split into an OS-independent core
plus a thin per-OS layer**, with XP rebuilt on the core first. Track M10,
design in doc 19.

**Why a native driver.** The Glide + WineD3D stack Win98 uses today needs
per-game files in per-game folders and cannot accelerate the desktop at
all; the driver needs nothing next to a game, because 98's own
`ddraw.dll` / `d3dim.dll` drive it. It is also the cheap half of a job
already done: the DirectX 3–7 behaviour the 98 title matrix depends on —
execute buffers, colour keys, palettized textures, 8 bpp modes, the flip
chain's vertical blank — is exactly what M7 spent itself on for XP.

**Confirmed by step 0 (2026-09-06).** Reading `vmdisp9x` and `vmhal9x`
settled the premises rather than leaving them assumed: the per-call DDI
structures are field-for-field identical to NT's (`D3DHAL_` vs
`D3DNTHAL_DRAWPRIMITIVES2DATA`: same fourteen fields, same order) and the
DP2 opcode values agree, so the walker is portable as it stands; the
DirectDraw *object* structures are laid out differently, so the neutral
descriptor is required rather than merely prudent; and DDI 8 works on 9x,
so the whole of M7c is in scope. The one structural surprise is that on
9x the DirectDraw/Direct3D HAL is a **ring-3 DLL in the game's process**,
not part of the display driver — which is why the core may call no
operating-system service at all: it is linked into a kernel-mode DLL on
NT and a user-mode one on 9x. Details in doc 19.

**Why the core, and why first.** `d3dptdisp.c` is 3 708 lines of which
roughly three quarters — the DP2 walker, the surface table and its format
arithmetic, the caps tables, contexts and readback, the flip chain, the
encoder and the debug log — states facts about *our adapter and our
protocol*, not about NT. Copying that into a second tree would double
every future protocol bump and guarantee the two drivers drift apart at
the first bug fixed on one side only. The core must not see either OS's
DirectDraw structures: the per-OS layer fills a neutral surface
descriptor, so NT's `DD_SURFACE_LOCAL` and 9x's `DDRAWI_DDRAWSURFACE_LCL`
stay behind the boundary even if they turn out to agree field for field.

**Sequencing.** The split lands on its own, proven to be a refactor by
the M7 suite being byte-identical across it (the same golden BMPs, the
same case counts, the same games drawing). Only then does the 9x layer
start — a 9x bug on top of an unproven refactor is two bugs wearing one
coat.

**What this does not change.** The M4 paravirtual device and its guest
DLLs stay exactly as they are and remain the path wherever the driver is
not installed. The adapter itself should need no change for 9x; if it
does, that is a finding worth writing down rather than a licence to fork
the register set.
