2ksbox guest tools - built from qemu-3dfx commit @REV@
(must match the host QEMU build's sign_commit stamp)

RUN SETUP.EXE. It knows which of these files your Windows wants, copies
them, registers what has to be registered, and writes SETUP.LOG. From a
command prompt or the Run box:  D:\SETUP.EXE
  SETUP /ALL             install everything this Windows can use
  SETUP /LIST            print the lists below and exit
  SETUP /GAME <n> <dir>  copy one file set next to a game's EXE

What is on the disc, if you would rather do it by hand:

GLIDE\    the device mapper and the 3dfx Glide wrappers.
          Windows 98/Me: GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD
                         -> C:\WINDOWS\SYSTEM
          2000/XP:       GLIDE*.DLL -> system32, FXPTL.SYS ->
                         system32\drivers, then run INSTDRV.EXE as
                         Administrator.
          The mapper is not optional for OPENGL32.DLL or for D3DPT\:
          they reach the device through it, and without it they refuse
          to load (0xc0000142 / "failed to initialize" on 2000/XP).

DRIVER\   the 2000/XP display driver for the paravirtual adapter. Boot
          the machine with -vga none -device d3dpt-vga, then run
          DRVINST.EXE -reboot. DRIVER\README.TXT lists its own test
          programs (DDTEST, D3D7TEST, SHTEST, ...). Not for Windows 9x,
          which runs the emulated Cirrus adapter.

D3DPT\    Direct3D 8/9 through the paravirtual device, per game: copy
          D3D8.DLL / D3D9.DLL next to the game's EXE. DDRAW.DLL as well
          when a launcher checks video memory through DirectDraw (GTA
          Vice City: "cannot find enough available video memory") - it
          answers 256 MB and forwards the rest to Windows' own.
          DINPUT.DLL fixes "the keyboard does nothing in the game" when
          the game polls a non-exclusive DirectInput keyboard from a loop
          that never pumps messages (FIFA 2000's match): what Windows
          reports pressed is merged into the state. Log: d3dpt.log next
          to the EXE. Never mix these with WINED3D\ in one folder.

OPENGL\   OPENGL32.DLL, the OpenGL pass-through wrapper: next to an
          OpenGL game's EXE (Quake 2 and friends). TESTS\WGLGEARS.EXE in
          the same folder is the two-second check that it works.

WINED3D\  Direct3D 8/9 -> OpenGL in the guest (wine9x @WINE9X@), the
          fallback for what the two stacks above do not cover. Per game:
          WINED3D.DLL plus WINED9.DLL renamed to D3D9.DLL (or WINED8 ->
          D3D8, WINEDD -> DDRAW). SETUP.EXE's /GAME does the renaming.
          The *_98 / *_XP switcher DLLs are for a system-wide install -
          read WINE9X.TXT first, it replaces files in system32.

TESTS\    every test, benchmark and calibration program on the disc, one
          copy each; SETUP puts them in C:\2KSBOX. A test that has to run
          on a particular stack needs that stack's DLLs beside it - copy
          the EXE into a folder of its own and use SETUP's /GAME there.
            D3DGAME9 D3DGAME8   the reference scene (doc 14). -frames N
                                runs a fixed sequence, -dump N x.bmp
                                writes a frame. Run these on real
                                hardware first: those BMPs are what the
                                emulated paths are compared against.
            D3DFEAT9            shaders, queries, cube maps, state blocks
            D3D9TEST            adapter, caps, x87 control word, a
                                spinning triangle with fps
            DDVMTEST            what a video-memory check sees
            MODETEST            the display modes the driver offers -
                                run it when a fullscreen game dies at
                                startup
            WGLGEARS            OpenGL, next to OPENGL32.DLL
            SSEBENCH            SSE and x87 throughput in ns per op
            CDTEST              CD audio through MCI
            CRTCAL              the CRT calibration patterns, exclusive
                                full-screen at the exact mode
            TEXTCAL.COM         the 720x400 text-mode patterns (DOS only)

CDSHELF\  the host's disc shelf, from inside the machine. CDSHELF.EXE on
          98/2000/XP, CDSHELF.COM in a DOS box; nothing to install (SETUP
          just puts the EXE where the Run box can find it). With no
          arguments the EXE opens a window and the COM lists the shelf
          and waits for a key. Either also takes a command:
            CDSHELF LIST   print the shelf and exit
            CDSHELF 3      put slot 3 in the drive
            CDSHELF E      empty the drive
          An insert always empties the drive first and waits for it -
          without that, Windows and MSCDEX keep showing the old disc.

Not included: GLIDE2X.OVL (DOS Glide games; needs Open Watcom to build).
