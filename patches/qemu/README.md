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

Currently empty — M0 carries no patches of our own.
