# Khronos OpenGL headers (vendored)

`GL/glcorearb.h` and `KHR/khrplatform.h` from the Khronos OpenGL registry
(https://registry.khronos.org/OpenGL/), MIT-licensed (see the SPDX header in
each file). Fetched 2026-09-02.

Why: qemu-3dfx's `hw/mesa/mesagl_pfn.h` includes `<GL/glcorearb.h>`. Linux
gets it from libglvnd/mesa; macOS has no `GL/` headers outside XQuartz.
`scripts/configure-qemu.sh` adds this directory to the include path on every
platform so the build sees one known header version.
