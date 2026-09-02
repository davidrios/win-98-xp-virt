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

**CRT:** Win9x has no UCRT, and modern mingw-w64 (Homebrew, Arch) links the
UCRT by default (`api-ms-win-crt-*.dll` imports → "required DLL not found"
on Win98). The script forces classic `msvcrt.dll` through a compiler shim
(`-D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os`) and refuses to package
anything that still imports UCRT api-sets.

**ISA:** upstream compiles the wrappers `-march=x86-64-v2` (SSE4.2/POPCNT)
and expects `-cpu host`/`max`; our reference guests are `pentium3`, so the
shim appends `-march=pentium3` and the script rejects binaries containing
SSE2+ instructions ("invalid instruction in module opengl32.dll" otherwise).

Needs a mingw32 cross toolchain + gendef/xxd/shasum and an ISO tool
(Arch: `mingw-w64-gcc mingw-w64-tools xorriso`; macOS: `brew install
mingw-w64 xorriso`). Homebrew's mingw-w64 ships no `gendef`; the script
builds it from pinned mingw-w64 sources into `out/tools/bin` when it is
missing (`GENDEF_FORCE_BUILD=1` forces that path). DOS-only pieces
(GLIDE2X.OVL, DJGPP DXEs) are skipped.
`WGLGEARS.EXE` is Mesa's wglgears from qemu-3dfx's demos — run it in the
guest next to `OPENGL32.DLL` as the zero-dependency GL pass-through check.

Later: SoftGPU (pinned release), AC'97/net drivers, XP D3D wrapper set, the
in-guest `verify` tool (docs 04/06).
