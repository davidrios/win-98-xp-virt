# Direct3D golden captures

Frames from the reference workloads (`D3DGAME9.EXE` / `D3DGAME8.EXE`,
`guest-tools/src`, doc 14 P0a) on the reference rig: P4 1.7 + GeForce 6200
(ForceWare `nv4_disp.dll`), Windows XP SP3, 85 Hz CRT. Every emulated
Direct3D path (WineD3D in the guest today, the paravirtual device
tomorrow) is diffed against these with `tools/bmpdiff.py`:

```sh
tools/bmpdiff.py reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp candidate.bmp -o diff.bmp
```

BMPs are kept as the tool writes them (24-bit, 640×480, ~900 KB) so a
byte-identical candidate compares equal without any conversion step.

## rig-2026-09-03

Captured 2026-09-03 (the rig's clock reads 2026-09-04; it is a day ahead).
The EXEs on the ISO were built from commit 667ecac.

| File | Command line | Notes |
|---|---|---|
| `d3dgame9-w300-ff.bmp` | `D3DGAME9 -frames 600 -dump 300 d3d9_dump.bmp` | windowed 640×480 X8R8G8B8, hardware vertex processing, fixed function |
| `d3dgame9-w300-vs11.bmp` | `D3DGAME9 -shader -frames 600 -dump 300 d3d9_dump_shader.bmp` | same, cubes through the vs_1_1 vertex shader **with the fixed-function pixel stage**: the rig's d3dx9_36 HLSL compiler refuses ps_1_1 (X3539), so CreatePixelShader never ran. The log of that build says "fixed function" for this case; it lies, `draw_cubes` keys on the vertex shader alone (the log names all three cases since the same day). **Rendering is frozen at this build**: emulated runs must draw exactly what the rig drew, so the ps_1_1 rejection stays as it is until a new golden set is taken. |
| `d3dgame9.log`, `d3dgame8.log` | all runs of the session | adapter/caps lines, fps per second; the "N frames, M ms" summaries of this build are wrong (they measured since the last fps report), fixed the same day (log only, no pixel changes) |

**HUD caveat:** the frame-time bars (bottom left) draw wall time, so they
differ between any two runs, on the rig too; always mask them when diffing
(`--mask 0,368,270,112`). Between the two files
above only the cubes (the shader path) and the HUD differ; the diff tool
reports 9.7 % of pixels with the HUD masked.

Performance seen on the rig (vsync on, 85 Hz monitor; `-novsync` was
not run):

| Run | fps |
|---|---|
| d3dgame9 windowed / fullscreen 8888 / fullscreen 565 | 85 (vsync) |
| d3dgame8 fullscreen | 85 |
| d3dgame8 windowed (`D3DSWAPEFFECT_COPY_VSYNC`) | 43–44: the GeForce driver paces a windowed COPY_VSYNC present at half the refresh rate. Real behaviour, keep it in mind when reading d3dgame8 windowed numbers. |
