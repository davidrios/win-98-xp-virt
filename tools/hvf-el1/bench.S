// hvf-el1: the load kernels, assembled into both the EL1 payload (ELF) and
// the host baseline (Mach-O), so the same instructions run natively and in
// the VM. Labels carry the Mach-O underscore; the ELF side binds them with
// link_name.
//
// "direct"  = what translated code would do with the guest's page tables
//             mirrored in stage 1: the guest linear address is an offset
//             from a window base register (x86 addresses are 32-bit).
// "softmmu" = the sequence tcg/aarch64 emits today for a 32-bit guest load
//             (prepare_host_addr: ldp mask/table, index, comparator, addend,
//             compare, branch, load), against a TLB that always hits.
// "pinned"  = softmmu with the mask/table pair held in registers (what
//             patch 16's pinned registers could do without a hypervisor).
//
// Chase: x0 = base or env, x1 = first offset, x2 = steps  -> x0 = last offset
// Sum:   x0 = base or env, x1 = u32 offsets, x2 = count  -> x0 = sum
// env: [0] = mask ((n-1) << 5), [8] = table; entry: addr_read @0, addend @24

    .text
    .balign 64
    .globl _bench_chase_direct
_bench_chase_direct:
1:  ldr     w1, [x0, w1, uxtw]
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x1
    ret

    .balign 64
    .globl _bench_chase_softmmu
_bench_chase_softmmu:
1:  ldp     x3, x4, [x0]
    and     x3, x3, x1, lsr #7
    add     x4, x4, x3
    ldr     w5, [x4]
    ldr     x4, [x4, #24]
    add     w6, w1, #3
    and     w6, w6, #0xfffff000
    cmp     w5, w6
    b.ne    9f
    ldr     w1, [x4, w1, uxtw]
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x1
    ret
9:  mov     x0, #-1
    ret

    .balign 64
    .globl _bench_chase_pinned
_bench_chase_pinned:
    ldp     x7, x8, [x0]
1:  and     x3, x7, x1, lsr #7
    add     x4, x8, x3
    ldr     w5, [x4]
    ldr     x4, [x4, #24]
    add     w6, w1, #3
    and     w6, w6, #0xfffff000
    cmp     w5, w6
    b.ne    9f
    ldr     w1, [x4, w1, uxtw]
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x1
    ret
9:  mov     x0, #-1
    ret

    .balign 64
    .globl _bench_sum_direct
_bench_sum_direct:
    mov     x5, #0
1:  ldr     w3, [x1], #4
    ldr     w3, [x0, w3, uxtw]
    add     x5, x5, x3
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x5
    ret

    .balign 64
    .globl _bench_sum_softmmu
_bench_sum_softmmu:
    mov     x9, #0
    mov     x10, x1
1:  ldr     w1, [x10], #4
    ldp     x3, x4, [x0]
    and     x3, x3, x1, lsr #7
    add     x4, x4, x3
    ldr     w5, [x4]
    ldr     x4, [x4, #24]
    add     w6, w1, #3
    and     w6, w6, #0xfffff000
    cmp     w5, w6
    b.ne    9f
    ldr     w3, [x4, w1, uxtw]
    add     x9, x9, x3
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x9
    ret
9:  mov     x0, #-1
    ret

    .balign 64
    .globl _bench_sum_pinned
_bench_sum_pinned:
    mov     x9, #0
    ldp     x7, x8, [x0]
    mov     x10, x1
1:  ldr     w1, [x10], #4
    and     x3, x7, x1, lsr #7
    add     x4, x8, x3
    ldr     w5, [x4]
    ldr     x4, [x4, #24]
    add     w6, w1, #3
    and     w6, w6, #0xfffff000
    cmp     w5, w6
    b.ne    9f
    ldr     w3, [x4, w1, uxtw]
    add     x9, x9, x3
    subs    x2, x2, #1
    b.ne    1b
    mov     x0, x9
    ret
9:  mov     x0, #-1
    ret

// touch_pages(base, count, stride): one load per page -> x0 = xor of values
    .balign 64
    .globl _bench_touch
_bench_touch:
    mov     x4, #0
1:  ldr     w3, [x0]
    eor     x4, x4, x3
    add     x0, x0, x2
    subs    x1, x1, #1
    b.ne    1b
    mov     x0, x4
    ret

// ---------------------------------------------------------------- rep movsd
// The blit loop Moto Racer 1997 spends half its vCPU in (`rep movsd`, one
// per 640-byte row; docs/tracks/m9-tcg-aarch64.md): the shape of
// do_gen_rep + gen_movs (target/i386/tcg/translate.c) — per iteration a
// load, a store, esi/edi += 4, ecx = cx_next, the cc_op store to env,
// cx_next -= 1 and the REP_MAX test; the last iteration outside the loop.
// x0 = window base (direct/fast) or unused, x1 = src offset, x2 = dst
// offset, x3 = rows, x4 = dwords per row (>= 8, a multiple of 8), x5 = env
// (mask @0, table @8, cc_op @16; entry: addr_read @0, addr_write @8,
// addend @24) -> x0 = 0.  Rows are contiguous in both buffers.

    .balign 64
    .globl _bench_movs_direct        // the VM design: pinned registers, window accesses
_bench_movs_direct:
    mov     x9, x3
    mov     w12, #0x2a
2:  sub     w11, w4, #1              // cx_next
1:  ldr     w6, [x0, w1, uxtw]
    str     w6, [x0, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    mov     w10, w11                 // ecx
    str     w12, [x5, #16]           // cc_op
    sub     w11, w11, #1
    tst     w11, #0xffff
    b.ne    1b
    ldr     w6, [x0, w1, uxtw]       // last iteration
    str     w6, [x0, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    mov     w10, w11
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_movs_softmmu       // today: the softmmu chain before the load and the store
_bench_movs_softmmu:
    mov     x9, x3
    mov     w12, #0x2a
2:  sub     w11, w4, #1
1:  ldp     x13, x14, [x5]           // load: mask/table, index, comparator, addend, page check
    and     x13, x13, x1, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14]
    ldr     x14, [x14, #24]
    add     w16, w1, #3
    and     w16, w16, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    ldr     w6, [x14, w1, uxtw]
    ldp     x13, x14, [x5]           // store: the same against addr_write
    and     x13, x13, x2, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14, #8]
    ldr     x14, [x14, #24]
    add     w16, w2, #3
    and     w16, w16, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    str     w6, [x14, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    mov     w10, w11
    str     w12, [x5, #16]
    sub     w11, w11, #1
    tst     w11, #0xffff
    b.ne    1b
    ldp     x13, x14, [x5]           // last iteration
    and     x13, x13, x1, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14]
    ldr     x14, [x14, #24]
    add     w16, w1, #3
    and     w16, w16, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    ldr     w6, [x14, w1, uxtw]
    ldp     x13, x14, [x5]
    and     x13, x13, x2, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14, #8]
    ldr     x14, [x14, #24]
    add     w16, w2, #3
    and     w16, w16, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    str     w6, [x14, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    mov     w10, w11
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret
9:  mov     x0, #-1
    ret

    .balign 64
    .globl _bench_movs_fast          // a REP MOVS fast path: per run inside one page of the
_bench_movs_fast:                    // source and of the destination, one softmmu probe of
    mov     x9, x3                   // each, then a vector copy (rows are 16-byte aligned)
    mov     w17, #4096
2:  lsl     w10, w4, #2              // bytes left in the row
3:  and     w16, w1, #0xfff          // chunk = min(left, to the source page end, to the
    sub     w16, w17, w16            //             destination page end)
    cmp     w10, w16
    csel    w11, w10, w16, lo
    and     w16, w2, #0xfff
    sub     w16, w17, w16
    cmp     w11, w16
    csel    w11, w11, w16, lo
    ldp     x13, x14, [x5]           // source probe (read) -> x7 host address
    and     x13, x13, x1, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14]
    ldr     x14, [x14, #24]
    and     w16, w1, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    add     x7, x14, x1
    ldp     x13, x14, [x5]           // destination probe (write) -> x8
    and     x13, x13, x2, lsr #7
    add     x14, x14, x13
    ldr     w15, [x14, #8]
    ldr     x14, [x14, #24]
    and     w16, w2, #0xfffff000
    cmp     w15, w16
    b.ne    9f
    add     x8, x14, x2
    lsr     w12, w11, #4             // 16 bytes per step
1:  ldr     q0, [x7], #16
    str     q0, [x8], #16
    subs    w12, w12, #1
    b.ne    1b
    add     w1, w1, w11              // esi, edi
    add     w2, w2, w11
    subs    w10, w10, w11
    b.ne    3b
    mov     w10, #0                  // ecx
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret
9:  mov     x0, #-1
    ret

// "today": the loop tcg/aarch64 actually emits for this `rep movsd` under
// patch 09 (moto-blit-d/qemu-d.log, 2026-09-05), transcribed: x19 -> x5, the
// same env offsets (ecx @4, esi @0x18, edi @0x1c, eip @0x20, cc_op @0x34,
// df @0x38, mask/table @-0xf0), the same stack spills. The guest registers
// go through env at every iteration because the loop's back edge is a TCG
// basic-block boundary. `mb` = with the dmb barriers (1) or without (0).
// x5 here is the mask/table pointer; the kernel's env is x5 + 0xf0, so the
// caller keeps [x5 + 0xf0, x5 + 0x130) free.
.macro MOVS_TODAY_BODY mb
    .if \mb
    dmb     ishld
    .endif
    ldr     w20, [x5, #0x18]
    ldp     x16, x17, [x5, #-0xf0]
    and     w16, w16, w20, lsr #7
    add     x17, x17, x16
    ldr     w16, [x17]
    ldr     x17, [x17, #0x18]
    add     w30, w20, #3
    and     w30, w30, #0xfffff000
    cmp     w16, w30
    b.ne    9f
    ldr     w21, [x17, w20, uxtw]
    str     w21, [sp, #0x80]
    ldr     w22, [x5, #0x1c]
    str     w22, [sp, #0x8c]
    .if \mb
    dmb     ish
    .endif
    ldp     x16, x17, [x5, #-0xf0]
    and     w16, w16, w22, lsr #7
    add     x17, x17, x16
    ldr     w16, [x17, #8]
    ldr     x17, [x17, #0x18]
    add     w30, w22, #3
    and     w30, w30, #0xfffff000
    cmp     w16, w30
    b.ne    9f
    str     w21, [x17, w22, uxtw]
    ldr     w21, [sp, #0x84]
    add     w20, w20, w21
    str     w20, [x5, #0x18]
    add     w20, w22, w21
    str     w20, [x5, #0x1c]
    ldr     w20, [sp, #0x88]
    str     w20, [x5, #4]
.endm

.macro MOVS_TODAY name, mb
    .balign 64
    .globl \name
\name:
    sub     sp, sp, #0x100
    stp     x20, x21, [sp, #0xe0]
    stp     x22, x30, [sp, #0xf0]
    add     x5, x5, #0xf0
    str     w1, [x5, #0x18]
    str     w2, [x5, #0x1c]
    mov     w21, #1
    str     w21, [x5, #0x38]         // df
    mov     x9, x3
2:  str     w4, [x5, #4]             // the row's `mov ecx, edx` (its own TB)
    ldr     w20, [x5, #4]            // the rep TB: ecx
    ldr     w21, [x5, #0x38]
    lsl     w21, w21, #2
    str     w21, [sp, #0x84]         // dshift
    mov     w21, #0x2a
    str     w21, [x5, #0x34]         // cc_op
    cbz     w20, 8f
    sub     w20, w20, #1
    str     w20, [sp, #0x88]         // cx_next
    cbz     w20, 5f
1:  MOVS_TODAY_BODY \mb
    sub     w20, w20, #1
    str     w20, [sp, #0x88]
    tst     w20, #0xffff
    b.ne    1b
    cbz     w20, 5f
    b       9f                       // the REP_MAX chunk: rows are shorter
5:  MOVS_TODAY_BODY \mb              // the last iteration
    ldr     w20, [x5, #0x20]
    add     w20, w20, #0xb
    str     w20, [x5, #0x20]         // eip
8:  ldr     w1, [x5, #0x18]          // the next TB reloads esi/edi
    ldr     w2, [x5, #0x1c]
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    b       7f
9:  mov     x0, #-1
7:  ldp     x20, x21, [sp, #0xe0]
    ldp     x22, x30, [sp, #0xf0]
    add     sp, sp, #0x100
    ret
.endm

MOVS_TODAY _bench_movs_today, 1
MOVS_TODAY _bench_movs_today_nomb, 0

// diagnostics for the VM: the direct loop without the cc_op store, stores
// only, loads only
    .balign 64
    .globl _bench_movs_direct_nocc
_bench_movs_direct_nocc:
    mov     x9, x3
2:  mov     w11, w4
1:  ldr     w6, [x0, w1, uxtw]
    str     w6, [x0, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    subs    w11, w11, #1
    b.ne    1b
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_stos_direct
_bench_stos_direct:
    mov     x9, x3
    mov     w6, #0x5a5a
2:  mov     w11, w4
1:  str     w6, [x0, w2, uxtw]
    add     w2, w2, #4
    subs    w11, w11, #1
    b.ne    1b
    add     w1, w1, w4, lsl #2
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_lods_direct
_bench_lods_direct:
    mov     x9, x3
    mov     w7, #0
2:  mov     w11, w4
1:  ldr     w6, [x0, w1, uxtw]
    add     w7, w7, w6
    add     w1, w1, #4
    subs    w11, w11, #1
    b.ne    1b
    add     w2, w2, w4, lsl #2
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0
    ret

// VM diagnostics: rows*w iterations of one store/load pattern
    .balign 64
    .globl _bench_diag_stenv         // str to env+16 (the line holding mask/table)
_bench_diag_stenv:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [x5, #16]
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_stenv_far     // str to env+0x80 (another line)
_bench_diag_stenv_far:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [x5, #0x80]
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_ldst_env      // ldp mask/table then str to the same line
_bench_diag_ldst_env:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  ldp     x13, x14, [x5]
    add     x13, x13, x14
    str     w12, [x5, #16]
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, x13
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_ststack       // str to the stack
_bench_diag_ststack:
    sub     sp, sp, #32
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [sp, #16]
    subs    x9, x9, #1
    b.ne    1b
    add     sp, sp, #32
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_stwin_ldline  // str through the window, ldr from the same line
_bench_diag_stwin_ldline:
    mul     x9, x3, x4
    mov     w12, #0x2a
    add     w13, w2, #16
1:  str     w12, [x0, w2, uxtw]
    ldr     w14, [x0, w13, uxtw]
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_stenv_ldbase  // str env; ldr through base
_bench_diag_stenv_ldbase:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [x5, #16]
    ldr     w6, [x0, w1, uxtw]
    add     w1, w1, #4
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_stenv_stbase  // str env; str through base
_bench_diag_stenv_stbase:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [x5, #16]
    str     w12, [x0, w2, uxtw]
    add     w2, w2, #4
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret

    .balign 64
    .globl _bench_diag_stenv_ldstbase // str env; ldr + str through base (the copy)
_bench_diag_stenv_ldstbase:
    mul     x9, x3, x4
    mov     w12, #0x2a
1:  str     w12, [x5, #16]
    ldr     w6, [x0, w1, uxtw]
    str     w6, [x0, w2, uxtw]
    add     w1, w1, #4
    add     w2, w2, #4
    subs    x9, x9, #1
    b.ne    1b
    mov     x0, #0
    ret
