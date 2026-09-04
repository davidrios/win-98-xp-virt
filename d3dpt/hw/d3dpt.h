/*
 * d3dpt.h — the paravirtual Direct3D device (hw/d3dpt, doc 14). Overlaid
 * into qemu/hw/d3dpt by scripts/prepare-qemu.sh; patch 40 wires
 * d3dpt_mm_init() into the pc machine and the meson tree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_D3DPT_H
#define HW_D3DPT_H

#include <stdbool.h>
#include <stdint.h>

#define TYPE_D3DPT "d3dpt"

/* the presenter the embed library installs: frames from the executor's
 * readback (XRGB8888, top-down, stride in bytes; valid during the call) and
 * the 3D active state. Without one, frames are dropped (standalone QEMU). */
typedef struct D3dptPresentOps {
    void (*active)(bool on);
    void (*frame)(const void *pixels, int width, int height, int stride);
} D3dptPresentOps;

void d3dpt_mm_init(void);
void d3dpt_set_present_ops(const D3dptPresentOps *ops);

#endif
