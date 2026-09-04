/*
 * d3dpt_mm.c — the paravirtual Direct3D device model (doc 14, ADR-006).
 *
 * The qemu-3dfx transport shape: a SysBus device with a 4 KiB register
 * page at D3DPT_MM_BASE and a RAM window at D3DPT_SHM_BASE, both at fixed
 * guest-physical addresses the guest maps through FXPTL.SYS/FXMEMMAP.VXD.
 * The guest writes command records into the window and rings
 * D3DPT_REG_DOORBELL; the write handler executes the batch synchronously
 * on the vCPU thread (BQL held) through libd3dpt_exec (d3dpt/exec, C++
 * over DXVK), dlopened on the first attach (d3dpt_exec_load.c, shared
 * with the d3dpt-vga adapter) so a machine without the library (or
 * without Vulkan) boots and only reports D3DPT_STATUS_NO_EXEC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qom/object.h"

#include "hw/d3dpt/d3dpt.h"
#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

OBJECT_DECLARE_SIMPLE_TYPE(D3dptState, D3DPT)

struct D3dptState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion shm;
    uint8_t *shm_ptr;

    const D3dptExecLib *lib;
    d3dpt_exec_t *exec;
    bool exec_tried;
    bool active;
    uint32_t attach;
    uint32_t last_err;
};

static const D3dptPresentOps *present_ops;

void d3dpt_set_present_ops(const D3dptPresentOps *ops)
{
    present_ops = ops;
}

static void exec_log(void *ud, const char *msg)
{
    info_report("d3dpt: %s", msg);
}

static void exec_active(void *ud, int on)
{
    D3dptState *s = ud;
    s->active = on != 0;
    if (present_ops && present_ops->active) {
        present_ops->active(on != 0);
    }
}

static void exec_frame(void *ud, const void *px, int w, int h, int stride)
{
    static bool warned;
    if (present_ops && present_ops->frame) {
        present_ops->frame(px, w, h, stride);
    } else if (!warned) {
        warned = true;
        warn_report("d3dpt: no presenter in this build; frames are dropped");
    }
}

static bool exec_load(D3dptState *s)
{
    d3dpt_exec_ops ops = { s, exec_log, exec_active, exec_frame, NULL };

    if (s->exec_tried) {
        return s->exec != NULL;
    }
    s->exec_tried = true;
    s->lib = d3dpt_exec_lib();
    if (!s->lib) {
        return false;
    }
    s->exec = s->lib->create(&ops);
    if (!s->exec) {
        warn_report("d3dpt: executor refused to start (no DXVK / Vulkan device)");
        return false;
    }
    return true;
}

static uint64_t d3dpt_read(void *opaque, hwaddr addr, unsigned size)
{
    D3dptState *s = opaque;

    switch (addr) {
    case D3DPT_REG_MAGIC:
        return D3DPT_MAGIC;
    case D3DPT_REG_VERSION:
        return D3DPT_PROTO_VERSION;
    case D3DPT_REG_STATUS:
        /* probing loads the executor: the answer must be honest */
        return exec_load(s) ? D3DPT_STATUS_READY : D3DPT_STATUS_NO_EXEC;
    case D3DPT_REG_DOORBELL:
        return s->last_err;
    case D3DPT_REG_ATTACH:
        return s->attach;
    default:
        return 0;
    }
}

static void d3dpt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    D3dptState *s = opaque;

    switch (addr) {
    case D3DPT_REG_DOORBELL:
        if (s->exec) {
            s->last_err = s->lib->submit(s->exec, s->shm_ptr, D3DPT_SHM_SIZE);
        } else {
            d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)s->shm_ptr;
            s->last_err = D3DPT_ERR_HOST;
            h->ret_status = D3DPT_ERR_HOST;
            h->cmd_bytes = 0;
            h->cmd_count = 0;
        }
        break;
    case D3DPT_REG_ATTACH:
        if (val) {
            if (exec_load(s)) {
                s->attach++;
                s->lib->attach(s->exec, 1);
            }
        } else if (s->attach) {
            s->attach--;
            s->lib->attach(s->exec, 0);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps d3dpt_ops = {
    .read = d3dpt_read,
    .write = d3dpt_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void d3dpt_reset(DeviceState *dev)
{
    D3dptState *s = D3DPT(dev);

    /* guest reboot: every attached process is gone */
    while (s->attach) {
        s->attach--;
        s->lib->attach(s->exec, 0);
    }
    s->last_err = 0;
    memset(s->shm_ptr, 0, sizeof(d3dpt_shm_hdr));
}

static void d3dpt_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    D3dptState *s = D3DPT(obj);

    memory_region_init_ram(&s->shm, obj, "d3dpt-shm", D3DPT_SHM_SIZE, &error_fatal);
    s->shm_ptr = memory_region_get_ram_ptr(&s->shm);
    memory_region_add_subregion(get_system_memory(), D3DPT_SHM_BASE, &s->shm);

    memory_region_init_io(&s->iomem, obj, &d3dpt_ops, s, TYPE_D3DPT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void d3dpt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, d3dpt_reset);
    dc->desc = "paravirtual Direct3D device (win98-xp-virt)";
    dc->user_creatable = false;
}

static const TypeInfo d3dpt_info = {
    .name = TYPE_D3DPT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(D3dptState),
    .instance_init = d3dpt_init,
    .class_init = d3dpt_class_init,
};

static void d3dpt_register_types(void)
{
    type_register_static(&d3dpt_info);
}

type_init(d3dpt_register_types)

void d3dpt_mm_init(void)
{
    DeviceState *dev = qdev_new(TYPE_D3DPT);

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, D3DPT_MM_BASE);
}
