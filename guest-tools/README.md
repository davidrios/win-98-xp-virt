# guest-tools

Build scripts for the per-guest driver/tools ISOs (design doc 04). Populated
in M3 (Win98) and M4 (XP). Contents will include SoftGPU (pinned release),
qemu-3dfx guest wrappers **built from the same `third_party/qemu-3dfx` commit
as the host build** (sign_commit enforces this), AC'97/net drivers, and the
in-guest `verify` acceptance tool. Era binaries are fetched/pinned, never
committed.
