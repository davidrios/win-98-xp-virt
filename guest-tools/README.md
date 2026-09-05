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
          DDRAW.DLL WINED3D.DLL                            → next to a DirectX 5–7 game's EXE (DirectDraw / Direct3D ≤7 via WineD3D)
WINED3D\  the full WineD3D set + per-OS switcher DLLs       → system-wide install, see WINE9X.TXT
D3DPT\    D3D9.DLL D3D8.DLL + D3D9TEST D3DGAME9 D3DFEAT9 D3DGAME8 → paravirtual Direct3D 8/9 (our device, doc 14): D3D9.DLL or D3D8.DLL next to the game's EXE; needs FXPTL.SYS / FXMEMMAP.VXD like OPENGL32.DLL. Never together with WINED3D's D3D9.DLL. Log in d3dpt.log next to the EXE (C:\d3dpt.log from the CD)
CDSHELF\  CDSHELF.EXE (98/2000/XP) CDSHELF.COM (DOS)       → the host's disc shelf from inside the machine; copy anywhere and run
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

**wine9x patch queue** (`patches/wine9x/*.patch`, git-format diffs against
the pinned commit, applied by the script after checkout):
- `01-24bit-desktop-mode`: wined3d maps both 24- and 32-bit desktops to
  `B8G8R8X8`, so a fullscreen swapchain created from a 24-bit desktop asks
  `ChangeDisplaySettingsEx` for 32 bpp; drivers without 32-bit modes (QEMU
  cirrus on XP at 800×600) refuse, the device never comes up and Wine 1.7.55
  crashes in its own error path (`glsl_fragment_pipe_free` on a NULL priv).
  Found with FIFA 2000 on XP, 2026-09-03: now a 32-bpp request on a 24-bpp
  desktop stays at 24 (redundant when the size matches), and a failed 32-bpp
  switch is retried at 24. Worth sending upstream to JHRobotics.

- `02-debug-log-flush`: Wine's log writer buffers; a crash loses the tail.
  Flush per line (no effect on release builds, which compile logging out).

**Debug build of the WineD3D set** (logs that survive a crash): a second
checkout of wine9x with the same patches, built with `SPEED= WINED3D_SILENT=`
and the same `CC`/`LD`/`LIBSTATIC` overrides as the script (see
`build_wined3d`); its `wined3d.dll`/`winedd.dll`/`ddraw_xp.dll` write
`proc_<pid>_dwine.log` and `proc_<pid>_wined3d.log` into the game folder
(err/fixme/warn by default, `set WINEDEBUG=+ddraw,+d3d` for traces). On a
shut-down guest the logs are read from the qcow2 on the host:
`qemu-img convert -O raw` → `hdiutil attach -readonly -nomount
-imagekey diskimage-class=CRawDiskImage` → `diskutil mount readOnly`
(NTFS is read-only on macOS); Dr Watson's `drwtsn32.log` (UTF-16) names the
faulting module. `guest-tools/out/wined3d-debug.iso` was such a set
(2026-09-03); rebuild it the same way when needed.

**CDSHELF — the host's disc shelf, from inside the machine** (doc 07's
shelf, `cdshelf/cdshelf_proto.h`, device side patch 52). `CDSHELF` lists
the discs the user keeps on the launcher's shelf, `CDSHELF <n>` puts one
in the drive, `CDSHELF E` empties it — no launcher window, no host
keyboard, from a guest that may be mid-game.

Both builds talk to the machine's *own CD-ROM drive*, which answers a
vendor ATAPI opcode with the shelf: that is the one channel DOS, Win98
and XP can all reach, and we own the drive's firmware, so there is no new
device and nothing to install in the guest.

- `CDSHELF.EXE` (`guest-tools/src/cdshelf.c`) — one binary for both
  Windows families. XP/2000 use SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT` on
  `\\.\<letter>:`); Win98/Me use ASPI, with `WNASPI32.DLL` loaded at *run
  time* (it does not exist on XP, and linking it would make the EXE
  unloadable there). Every CD-ROM drive is asked for the shelf and the
  one that answers is used, so a machine with two drives needs no
  argument; `-d E:` overrides on XP, `-v` shows each CDB. Writes
  `cdshelf.log` next to itself when it can.

  **With no arguments it opens a window** — a list of the shelf with
  Insert / Eject / Refresh — because swapping a disc is something you do
  while a game is asking for disc 2, not a command line you retype. Plain
  USER32 controls created in code (no resource file, nothing newer than
  Windows 95), and the insert runs on a worker thread so the window keeps
  painting while the drive settles. The verbs below still work for
  scripting, and still write to a redirected stdout even though the EXE
  is `-mwindows`.
- `CDSHELF.COM` (`guest-tools/src/cdshelf.asm`) — the DOS build, NASM,
  driving the drive by PIO exactly as `tools/atapi-guest-test.py` does
  (there is no DOS C toolchain in this build). It finds the drive with
  IDENTIFY PACKET DEVICE rather than by reading the ATAPI signature out
  of the cylinder registers: by the time a DOS program runs, the BIOS has
  long since detected the drive and left those at zero.

  DOS gets no window, so with no arguments it prints the shelf and waits
  for a key: **0-9 puts that disc in the drive**, `E` empties it, `R`
  re-reads the shelf, Esc quits, and the listing is reprinted after each
  one. `CDSHELF LIST` is the non-interactive form for a batch file or a
  redirect.

Both **empty the drive before inserting**, and wait for the drive to
confirm the tray is empty before loading. Two reasons: Windows and MSCDEX
cache what they last saw, so a swap they never observed as a removal
leaves the old disc's files on screen; and the device runs the medium
change from a single bottom half (patch 52), so an eject and a load sent
back to back without waiting collapse into one.

Both are exercised on every `tools/atapi-guest-test.py` run: the opcode
itself through the test's own PIO program, and then `CDSHELF.COM` for
real on a FreeDOS floppy, its output captured over COM1. `CDSHELF.EXE`
was verified by hand in a real XP guest (see the M6 track doc).

`MODETEST.EXE` (`guest-tools/src/modetest.c`) prints the current desktop
mode, the driver's mode list and the result of the `ChangeDisplaySettingsEx`
calls ddraw/wined3d make; run it when a fullscreen game dies at startup.

**Reference workloads (doc 14 P0a):** `D3DGAME9.EXE` / `D3DGAME8.EXE`
(`guest-tools/src/d3dgame9.c`, `d3dgame8.c`, shared scene in `d3dgame.h`)
draw the same deterministic game-like scene on both APIs: checker ground
with mipmaps, five lit indexed cubes with materials, a per-frame dynamic
vertex buffer (waving grid, 565 texture), additive DXT1 particles via
DrawPrimitiveUP, a render-to-texture monitor, a frame-time bar graph;
windowed or `-fs` exclusive fullscreen (`-w -h -bpp16 -novsync`), keyboard
camera (WASD/arrows/Q/E), F1 wireframe, Space pause, Esc quits; `-shader`
adds a vs_1_1/ps_1_1 path on d3d9 when a d3dx9 DLL is present (the vertex
shader is HLSL, the pixel shader is assembled: d3dx9_33+ HLSL compilers
refuse ps_1_x, X3539). `-frames N`
runs a fixed-step sequence with an auto-orbiting camera and exits;
`-dump N file.bmp` writes frame N. Everything printed also goes to
`d3dgame9.log` / `d3dgame8.log` next to the EXE (appended, flushed per
line; `-log file` to choose): a dated header, the arguments, the Windows
version, adapter and caps, the device configuration, shader compiler output,
fps once a second, dumps and errors. Copy the log along with the BMPs.
Procedure: run on the reference rig
(P4 + GeForce 6200) first — it must be flawless there — keep its BMPs as
golden images (`reference/d3d/`, first set 2026-09-03), then run the same
command lines under each emulated path and diff with `tools/bmpdiff.py`.
No commercial game, no disc, no crack involved.

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
