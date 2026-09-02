# QEMU patch queue

Our own patches on top of the pinned QEMU submodule (v9.2.4 — pinned to the
newest release qemu-3dfx's `00-qemu92x` patch supports; their reference is
9.2.2, ours applies clean on 9.2.4).

- qemu-3dfx overlay + patch come from `third_party/qemu-3dfx` and are applied
  by `scripts/prepare-qemu.sh` — they do NOT live in this queue.
- This queue holds *our* patches, numbered `NN-name.patch`, applied in order
  after prepare-qemu.sh. Planned series:
  - `10-embed-api` — `libqemu_embed.h` + library build (M1)
  - `20-atapi-libdisc` — raw-CD ATAPI device calling libdisc (M5)
- Every patch is written upstream-style (see doc 02: upstream-first is the
  fork-maintenance exit strategy).

Applied automatically by `scripts/prepare-qemu.sh` after the qemu-3dfx
overlay (`git apply`, idempotent via reverse-check — fine as long as a patch
doesn't touch files `sign_commit` edits: `hw/3dfx/g2xfuncs.h`,
`hw/mesa/mglfuncs.h`, `system/vl.c`).

Current queue:
- `01-upstream-i386-lss-tb-exit-fix.patch` — backport of upstream
  0f1d6606c28d fixing QEMU issue 2987 (TCG regression: Windows 98 SE
  exception 0D on first boot after setup; 9.2.4 carries the regressing LSS
  change but not the fix). Drop at QEMU >= 10.1.
- `00-3dfx-darwin-contextalpha.patch` — upstream qemu-3dfx Darwin build
  regression (`GL_CONTEXTALPHA` only defined under `CONFIG_LINUX`). Report
  upstream; drop when fixed there.
