# guest-tools

Per-guest driver/tools media (design doc 04). Era binaries are built or
fetched at build time, never committed (`out/` is git-ignored).

## build-wrappers.sh — qemu-3dfx guest wrappers (P2, first piece)

Builds the Windows guest wrappers from **the same `third_party/qemu-3dfx`
commit the host QEMU is signed with** (the host verifies the stamp; a
mismatch = no acceleration), and stages a `guest-tools-3dfx-<rev>.iso`:

```
WIN9X\    GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD   → C:\WINDOWS\SYSTEM
WIN2KXP\  GLIDE*.DLL FXPTL.SYS INSTDRV.EXE                 → system32 (+drivers), run INSTDRV as admin
GAMEDIR\  OPENGL32.DLL WGLGEARS.EXE                        → next to each OpenGL game's EXE
```

Needs a mingw32 cross toolchain + gendef/xxd/shasum and an ISO tool
(Arch: `mingw-w64-gcc mingw-w64-tools xorriso`; macOS: `brew install
mingw-w64 xorriso`). DOS-only pieces (GLIDE2X.OVL, DJGPP DXEs) are skipped.
`WGLGEARS.EXE` is Mesa's wglgears from qemu-3dfx's demos — run it in the
guest next to `OPENGL32.DLL` as the zero-dependency GL pass-through check.

Later: SoftGPU (pinned release), AC'97/net drivers, XP D3D wrapper set, the
in-guest `verify` tool (docs 04/06).
