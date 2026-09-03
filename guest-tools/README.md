# guest-tools

Per-guest driver/tools media (design doc 04). Era binaries are built or
fetched at build time, never committed (`out/` is git-ignored).

## build-wrappers.sh — qemu-3dfx guest wrappers (P2, first piece)

Builds the Windows guest wrappers from **the same `third_party/qemu-3dfx`
commit the host QEMU is signed with** (the host verifies the stamp; a
mismatch = no acceleration), and stages a `guest-tools-3dfx-<rev>.iso`:

```
WIN9X\    GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD   → C:\WINDOWS\SYSTEM
WIN2KXP\  GLIDE*.DLL FXPTL.SYS INSTDRV.EXE                 → system32 (+drivers), run INSTDRV as admin — needed for OPENGL32.DLL too (NT maps the device via FXPTL.SYS)
GAMEDIR\  OPENGL32.DLL WGLGEARS.EXE                        → next to each OpenGL game's EXE
          D3D9.DLL D3D8.DLL WINED3D.DLL D3D9TEST.EXE       → next to each Direct3D 8/9 game's EXE (WineD3D)
WINED3D\  the full WineD3D set + per-OS switcher DLLs       → system-wide install, see WINE9X.TXT
```

**WineD3D (Direct3D 8/9 → OpenGL → pass-through):** built from
[JHRobotics/wine9x](https://github.com/JHRobotics/wine9x) (Wine 1.7.55
with 9x/XP fixes, LGPL; the same author as SoftGPU), pinned by commit in
the script. `wined3d.dll` renders through whatever `opengl32.dll` the
loader finds first, i.e. the qemu-3dfx wrapper in the game folder; XP
needs OpenGL 2.1 with BGRA from the host, which the Air's Metal GL 2.1
provides. The per-game copies in `GAMEDIR\` are the Wine DX interfaces
under their real names (`wined9.dll` → `D3D9.DLL`), which is the
low-risk way to use them: nothing in system32 changes. The `WINED3D\`
folder has the system-wide variant (switcher DLLs that route each EXE to
Wine or to Microsoft's DLLs by registry, `HKLM\Software\DDSwitcher`)
for games that load D3D from elsewhere; that install replaces system32
files and needs safe mode + dllcache on XP, wine9x's README (`WINE9X.TXT`
on the ISO) has the steps. Wine's d3d8/d3d9 set the x87 to 24-bit
precision on CreateDevice like native Direct3D, which is what QEMU's
inline x87 mode 2 (doc 13) covers. Build notes: wine9x links the CRT
(msvcrt through the same shim), its pthread9x sub-build hardcodes `ar`
(overridden to the mingw one), `-march=pentium3` from the shim wins over
its `-march=pentium2`; the ISA and CRT checks run on its DLLs too.

`D3D9TEST.EXE` (`guest-tools/src/d3d9test.c`) is the D3D9 counterpart of
wglgears: prints the adapter identifier (WineD3D reports a GL-derived
card name), HAL caps, the x87 control word after CreateDevice (`PC=24`
expected), then spins a triangle and prints the fps every second; an
optional argument is the number of frames to run before exiting.

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

Later: SoftGPU (pinned release), AC'97/net drivers, the in-guest
`verify` tool (docs 04/06).
