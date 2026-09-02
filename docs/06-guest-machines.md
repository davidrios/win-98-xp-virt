# 6. Guest machines: Win98 and XP reference configs

The frontend ships two "machine families" with tested defaults. These are the
reference definitions the guided creation flow instantiates; users supply
their own OS media and licenses.

## Windows 98 SE machine

Modeled as a ~1998–2000 consumer PC.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX + PIIX) | period-correct chipset, best-tested with 9x |
| CPU model | `pentium3` (TCG) / host-masked (KVM) | avoids CPUID features 9x mishandles; sidesteps the fast-CPU Win9x bugs (e.g. the >2.1 GHz-class IOS/NDIS crashes). **Floor is pentium3 (SSE1)**: our guest-tools wrappers are built `-march=pentium3` (upstream builds them x86-64-v2 and expects `-cpu host`/`max`) |
| RAM | 256 MB default, **≤ 512 MB hard cap** | 9x VCache breaks above ~512 MB without patches |
| Video | QEMU std VGA (bochs) | SoftGPU's primary target; clean mode behavior for our pipeline |
| Audio | SB16 (DOS-mode compat) + AC'97 | SB16 for DOS boxes/games, AC'97 driver in guest tools |
| Net | PCnet (AMD) | driver in-box on 98 |
| Storage | IDE HDD (qcow2) + our ATAPI CD | period-correct; no VirtIO for 9x |
| Input | PS/2 mouse + kbd; USB tablet optional | PS/2 relative mode for games (see doc 03) |
| Floppy | enabled | driver/utility sneakernet, boot disks |

Known QEMU-side traps (tracked in `patches/qemu/README.md`): qemu-3dfx 3D
only activates with `-display sdl` (standalone QEMU) — the player path
needs the M3 context-provider work (Spike A doc); 9.2.4 TCG needs
the upstream LSS fix (issue 2987) or Win98 faults with exception 0D on first
boot; TCG also faults RUNDLL32 in Display Properties since 7.2 (issue 1964,
still open as of 2026-09 — cosmetic, the OS survives; KVM/WHPX unaffected).

Guest install notes (docs shipped with the app): install from user's CD image;
apply guest-tools ISO (SoftGPU, 3dfx wrappers, AC'97, unofficial fixes the
user opts into). **Install mode is pinned: ACPI (`SETUP /p j`).** A default (PnP-BIOS)
install in QEMU leaves the PCI bus un-enumerated — "Plug and Play BIOS"
with a yellow ! and no PCI hot-adds ever detected (USB tablet, AC'97, NIC).
The launcher's guided install must pass `/p j` (or drive the PnP-BIOS→PCI
Bus repair). Known quirks to document: DOS-compatibility-mode storage
regressions.

## Windows XP machine

Modeled as a ~2002–2005 PC.

| Component | Choice | Rationale |
|---|---|---|
| Machine | `pc` (i440FX) | best compat with XP-era drivers; q35 unnecessary |
| CPU | `pentium3`/host-class with sane flags | XP handles more, keep TCG features modest |
| RAM | 512 MB–1 GB default | period-typical, snappy |
| Video | std VGA + qemu-3dfx | XP inbox driver for 2D; wrappers for 3D |
| Audio | AC'97 (fallback: emulated HDA) | XP AC'97 driver in guest tools |
| Net | RTL8139 | in-box XP driver |
| Storage | IDE + our ATAPI CD | AHCI needs F6 drivers; not worth it |
| Input | PS/2 + USB tablet toggle | same grab semantics as 98 |

Notes: SP3 recommended; activation is the user's affair with their own
license (volume/retail as they possess) — the project ships nothing related
to it. TSC/HAL: uniprocessor ACPI HAL default; SMP under TCG is a measured
decision later (MTTCG helps, XP-era games rarely do).

## Performance expectations (set honestly in-app)

| Host | Win98 | XP |
|---|---|---|
| Linux/Windows x86 (KVM/WHPX) | vastly faster than period hardware | near-native |
| Apple Silicon (TCG) | comfortably faster than a period PC | usable; vs. the rig's P4 1.7 (M1 Air, 2026-09-02, `reference/benchmarks/`): boots as fast, integer 1.3–2× faster (7-Zip 0.996/1.511 vs 0.742/0.776 GIPS), x87 FP at 21 % (Super PI 1M 9:49 vs 2:02 — Pentium II class). State both in-app. |

The XP-on-Apple-Silicon row is the one architectural risk in the guest story;
it gets benchmarked in milestone M1, not discovered in M4. Baselines come from
the reference rig (P4 + GeForce 6200, doc 09): expectations are stated as a
percentage of that real machine's benchmark scores, not adjectives.

## Snapshots and storage

- qcow2 with named snapshots ("fresh install", "drivers installed",
  "pre-game-X") surfaced in the frontend — the retro workflow is
  reinstall-heavy and snapshots are the killer convenience.
- Machine definitions are declarative files (TOML/JSON) in the machine
  library; the player process translates to QEMU config. No user-visible
  QEMU command lines anywhere.
