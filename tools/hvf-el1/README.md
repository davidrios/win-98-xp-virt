# hvf-el1 — the Hypervisor.framework EL1 feasibility probe (M9)

Answers, with numbers from the Air, whether TCG's Arm output could run
inside a Hypervisor.framework VM with the x86 guest's page tables
mirrored in the VM's stage-1 tables (the "fastmem" idea of
`docs/tracks/m9-tcg-aarch64.md`), and what such a VM would have to
contain. It is a bare-metal aarch64 guest (Rust, `aarch64-unknown-none`,
37 KiB) plus a host program that creates the VM, shares a 64 MiB arena
with it at the same address in both worlds, and services its exits.

The guest brings up the MMU (4 KiB granule, 48-bit VA), installs
exception vectors, builds an x86-style two-level page table in "x86 RAM",
mirrors it lazily into a 4 GiB VA window from the data-abort handler (one
stage-1 root per x86 CR3, ASID-tagged), and measures:

- a VM exit (HVC, and an MMIO load through a stage-2 miss) against an
  in-VM exception and an in-VM function call;
- the first touch of a window page (abort → x86 walk → PTE → eret), the
  hardware TLB miss afterwards, an x86 #PF delivered to a resume point,
  the dirty-bit upgrade of a clean page, a CR3 switch with the tables
  kept vs. the flush-and-refault model;
- the load kernels of `bench.S` through the window vs. the exact sequence
  `tcg/aarch64` emits for a softmmu load (against a TLB that always hits),
  for working sets from 64 KiB to 32 MiB, dependent and independent —
  and the same kernels natively in the host process for the baseline;
- executing code written by the host process, self-patching without a
  W^X toggle, and the latency of a host-thread kick to the guest's IRQ
  handler;
- the `rep movsd` blit loop of a 2D game (`exp_movs` / `native_movs`,
  `results-movs-m1air-2026-09-05.txt`): TCG's actual loop transcribed
  from a `-d out_asm` log, the same with pinned guest registers, with
  direct window accesses, and a per-page-run probe + vector-copy fast
  path — with `env` in the identity map and inside the window, because
  a store to block-mapped memory followed by a store through a 4 KiB
  page mapping costs ~2 ns extra per pair in the VM (the `diag3` lines).

```sh
tools/hvf-el1/build.sh                     # payload (flat binary) + host, signed with hv.entitlements
build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin
build/hvf-el1/hvf-el1 x --native-only      # only the host baseline
```

macOS on Apple Silicon only (`kern.hv_support`); ~2 s. Every line is
`key: value` text; the results and their reading are in the track doc.
A reference run is `results-m1air-2026-09-05.txt`. Not part of `scripts/test.sh` — it is a measurement, not a regression
guard. On a guest fault the host prints the vCPU state (`dump`) and the
guest's own `ESR_EL1`/`FAR_EL1`/`ELR_EL1`; `llvm-objdump -d` on
`build/hvf-el1/payload/aarch64-unknown-none/release/payload` maps the
addresses.
