@echo off
rem GTA Vice City (DirectX 8, RenderWare) on the d3dpt-vga DX8 DDI (doc 15,
rem M7c): XP's own d3d8.dll on our driver, no wrapper DLL. The workload behind
rem the video-memory vertex buffers (protocol v9). Run headless by
rem   OUT=/tmp/claude-1000/vc CPU=pentium3 SHOTS=40 SHOT_KEYS="4:esc,6:esc,8:ret,10:ret,14:esc" \
rem     tools/xp-driver-test.sh <image> bat tools/xp-vicecity.bat
rem (the game lives in winxp-m7g; DDFLAGS=1048576 is the A/B: buffers in
rem system memory, every draw's vertices copied through the window). The
rem QEMU log's "ddi: N frames/s (... draws ...)" and "N page flips in 5.0 s"
rem lines are the numbers; tools/xp-vicecity.sh drives the menus into the
rem city and collects them.
cd /d "C:\Arquivos de programas\Rockstar Games\Grand Theft Auto Vice City"
if exist D3D8.DLL ren D3D8.DLL D3D8.PT
dir /b > E:\vcdir.txt
start /wait gta-vc.exe
echo exit %errorlevel% > E:\vcexit.txt
