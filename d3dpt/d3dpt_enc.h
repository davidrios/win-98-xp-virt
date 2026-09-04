/*
 * d3dpt_enc.h — the guest-side batch encoder of the paravirtual Direct3D
 * device, header-only C (no CRT beyond memcpy) so the same code runs in
 * the guest d3d9.dll (32-bit) and in the host test that drives the
 * executor without a guest. The doorbell callback is the only difference.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_ENC_H
#define D3DPT_ENC_H

#include <string.h>
#include "d3dpt_proto.h"

typedef struct d3dpt_enc {
    uint8_t *shm;                           /* the window (D3DPT_SHM_SIZE bytes) */
    void (*doorbell)(struct d3dpt_enc *e);  /* execute the batch synchronously */
    uint32_t ret_used;                      /* return-area allocator, reset per batch */
    uint32_t last_status;                   /* D3DPT_ERR_* of the last batch */
    uint32_t next_handle;
} d3dpt_enc;

static inline d3dpt_shm_hdr *d3dpt_enc_hdr(d3dpt_enc *e) { return (d3dpt_shm_hdr *)e->shm; }

static inline void d3dpt_enc_init(d3dpt_enc *e, uint8_t *shm, void (*doorbell)(d3dpt_enc *))
{
    e->shm = shm; e->doorbell = doorbell; e->ret_used = 0; e->last_status = 0; e->next_handle = 1;
    d3dpt_enc_hdr(e)->cmd_bytes = 0;
    d3dpt_enc_hdr(e)->cmd_count = 0;
}

static inline uint32_t d3dpt_enc_handle(d3dpt_enc *e) { return e->next_handle++; }

/* ring the doorbell if anything is pending; results stay readable until the
 * next record is appended */
static inline void d3dpt_enc_flush(d3dpt_enc *e)
{
    d3dpt_shm_hdr *h = d3dpt_enc_hdr(e);
    if (h->cmd_count) {
        e->doorbell(e);
        e->last_status = h->ret_status;
        h->cmd_bytes = 0;
        h->cmd_count = 0;
    }
    e->ret_used = 0;
}

/* append a record: op, fixed body of body_size, extra variable bytes;
 * returns the body pointer (caller fills body and tail) or NULL if the
 * record can never fit */
static inline void *d3dpt_enc_cmd(d3dpt_enc *e, uint32_t op, uint32_t body_size, uint32_t extra)
{
    d3dpt_shm_hdr *h = d3dpt_enc_hdr(e);
    uint32_t size = D3DPT_ALIGN8((uint32_t)sizeof(d3dpt_cmd) + body_size + extra);
    d3dpt_cmd *c;
    if (size > D3DPT_CMD_SIZE) return NULL;
    if (h->cmd_bytes + size > D3DPT_CMD_SIZE) d3dpt_enc_flush(e);
    c = (d3dpt_cmd *)(e->shm + D3DPT_CMD_OFFSET + h->cmd_bytes);
    c->op = op; c->size = size;
    /* zero the alignment tail so records are deterministic */
    if (size > sizeof(d3dpt_cmd) + body_size + extra)
        memset((uint8_t *)c + sizeof(d3dpt_cmd) + body_size + extra, 0, size - sizeof(d3dpt_cmd) - body_size - extra);
    h->cmd_bytes += size;
    h->cmd_count++;
    return c + 1;
}

/* reserve a return slot of d3dpt_ret + payload bytes; call BEFORE the
 * record that uses it (a flush here would otherwise run that record
 * with a slot from the previous batch) */
static inline uint32_t d3dpt_enc_ret(d3dpt_enc *e, uint32_t payload)
{
    uint32_t need = D3DPT_ALIGN8((uint32_t)sizeof(d3dpt_ret) + payload), off;
    if (e->ret_used + need > D3DPT_RET_SIZE) d3dpt_enc_flush(e);
    off = e->ret_used;
    e->ret_used += need;
    return off;
}
static inline d3dpt_ret *d3dpt_enc_result(d3dpt_enc *e, uint32_t off) { return (d3dpt_ret *)(e->shm + D3DPT_RET_OFFSET + off); }

/* --- convenience wrappers for the fixed-body ops --- */
static inline void d3dpt_enc_u32x2(d3dpt_enc *e, uint32_t op, uint32_t a, uint32_t b)
{
    d3dpt_u32x2 *p = (d3dpt_u32x2 *)d3dpt_enc_cmd(e, op, sizeof *p, 0);
    if (p) { p->a = a; p->b = b; }
}
static inline void d3dpt_enc_u32x3(d3dpt_enc *e, uint32_t op, uint32_t a, uint32_t b, uint32_t c)
{
    d3dpt_u32x3 *p = (d3dpt_u32x3 *)d3dpt_enc_cmd(e, op, sizeof *p, 0);
    if (p) { p->a = a; p->b = b; p->c = c; p->pad = 0; }
}
static inline void d3dpt_enc_u32x4(d3dpt_enc *e, uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    d3dpt_u32x4 *p = (d3dpt_u32x4 *)d3dpt_enc_cmd(e, op, sizeof *p, 0);
    if (p) { p->a = a; p->b = b; p->c = c; p->d = d; }
}
static inline void d3dpt_enc_nobody(d3dpt_enc *e, uint32_t op) { d3dpt_enc_cmd(e, op, 0, 0); }

/* sync record with just a handle: returns the HRESULT after the flush */
static inline uint32_t d3dpt_enc_sync(d3dpt_enc *e, uint32_t op, uint32_t handle)
{
    uint32_t off = d3dpt_enc_ret(e, 0);
    d3dpt_sync *p = (d3dpt_sync *)d3dpt_enc_cmd(e, op, sizeof *p, 0);
    if (!p) return 0x80004005u;   /* E_FAIL */
    p->handle = handle; p->ret_off = off;
    d3dpt_enc_flush(e);
    return e->last_status ? 0x80004005u : d3dpt_enc_result(e, off)->hr;
}

#endif /* D3DPT_ENC_H */
