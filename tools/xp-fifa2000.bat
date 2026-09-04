@echo off
rem FIFA 2000 on the d3dpt-vga Direct3D HAL (doc 15, M7c), run headless by
rem   GAME_ISO=FIFA2000.ISO SHOTS=24 tools/xp-driver-test.sh <image> bat tools/xp-fifa2000.bat
rem The game folder still carries the WineD3D set from the 2026-09-03 wine9x
rem experiments (DDRAW.DLL / WINED3D.DLL / WINEDD.DLL next to the EXE and in
rem THRASH\): renamed away so the game loads the system ddraw.dll -> our HAL.
rem The install path is the Brazilian XP's; the registry dump shows the
rem renderer choice (Thrash Driver dx = THRASH\dx6z.dll, Hardware Acceleration 1).
reg query "HKLM\Software\EA Sports\FIFA 2000" /s > E:\fifareg.txt
cd /d "C:\Arquivos de programas\EA SPORTS\FIFA 2000"
ren DDRAW.DLL DDRAW.WINE
ren WINED3D.DLL WINED3D.WINE
ren WINEDD.DLL WINEDD.WINE
cd THRASH
ren DDRAW.DLL DDRAW.WINE
ren WINED3D.DLL WINED3D.WINE
ren WINEDD.DLL WINEDD.WINE
cd ..
dir /b > E:\fifadir.txt
dir /b THRASH >> E:\fifadir.txt
start /wait fifa2000.exe
echo exit %errorlevel% > E:\fifaexit.txt
