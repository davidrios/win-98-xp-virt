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
