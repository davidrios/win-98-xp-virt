// hvf-el1 payload: entry, early page tables, MMU on, exception vectors.
//
// The host drops this image at IPA 0x40000000 and starts the vCPU at
// _start in EL1h with DAIF masked and the MMU off. Everything the guest
// does with the MMU off is here (aligned stores only: the memory is
// Device-nGnRnE until SCTLR_EL1.M is set).
//
// Stage-1 map built here (4 KiB granule, 48-bit VA, TTBR0 only):
//   VA 0x40000000 .. +4 MiB   image (two 2 MiB blocks)       Normal WB, RWX
//   VA <arena> .. +1 GiB      the arena shared with the host  Normal WB, RWX
//                             (host VA == IPA == guest VA: pointers are shared;
//                             the host passes the base in x0)
//   VA 0xC0000000 .. +1 GiB   "MMIO": Device; unmapped in stage 2 => VM exit
// The x86 windows (VA 0x1000000000 ..) are added by Rust at run time.

    .set ATTR_NORMAL_BLOCK, 0x701   // AF | SH=inner | AttrIdx0 | block
    .set ATTR_DEVICE_BLOCK, 0x405   // AF | AttrIdx1 | block
    .set TABLE, 0x3

// MAIR: idx0 = Normal WB RA/WA, idx1 = Device-nGnRnE, idx2 = Normal NC
    .set MAIR_VALUE, 0x0000000000004400ff
// TCR: T0SZ=16, IRGN0/ORGN0=WB, SH0=inner, TG0=4K, T1SZ=16, EPD1, IPS=40 bits
    .set TCR_VALUE, 0x0000000280903510
// SCTLR: RES1 | M | C | I
    .set SCTLR_VALUE, 0x30d01805

    .set FRAME_SIZE, 784

    .section .text.start, "ax"
    .globl _start
_start:
    // x0 = the arena's base (1 GiB aligned), from the host
    mov     x20, x0
    msr     daifset, #0xf
    mov     x0, #(3 << 20)              // CPACR_EL1.FPEN: FP/SIMD at EL1
    msr     cpacr_el1, x0
    isb
    ldr     x0, =vectors
    msr     vbar_el1, x0

    // zero .bss (page tables live there)
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    ldr     x0, =__stack_top
    mov     sp, x0

    // early tables: L0[0] -> L1; L1[1] -> L2img; L1[arena >> 30] = arena block; L1[3] = device block
    ldr     x0, =pt_l0
    ldr     x1, =pt_l1
    orr     x2, x1, #TABLE
    str     x2, [x0]
    ldr     x2, =pt_l2_img
    orr     x2, x2, #TABLE
    str     x2, [x1, #8]
    ldr     x2, =ARENA_BASE
    str     x20, [x2]
    mov     x2, #ATTR_NORMAL_BLOCK
    orr     x2, x20, x2
    lsr     x3, x20, #30
    str     x2, [x1, x3, lsl #3]
    ldr     x2, =(0xC0000000 | ATTR_DEVICE_BLOCK)
    str     x2, [x1, #24]
    ldr     x0, =pt_l2_img
    ldr     x2, =(0x40000000 | ATTR_NORMAL_BLOCK)
    str     x2, [x0]
    ldr     x2, =(0x40200000 | ATTR_NORMAL_BLOCK)
    str     x2, [x0, #8]
    dsb     sy

    ldr     x0, =pt_l0
    msr     ttbr0_el1, x0
    ldr     x0, =MAIR_VALUE
    msr     mair_el1, x0
    ldr     x0, =TCR_VALUE
    msr     tcr_el1, x0
    tlbi    vmalle1
    dsb     sy
    isb
    ldr     x0, =SCTLR_VALUE
    msr     sctlr_el1, x0
    isb
    ic      iallu
    dsb     sy
    isb

    bl      rust_main
    mov     x0, #0                      // HVC_EXIT
    hvc     #0
3:  wfe
    b       3b
    .ltorg

// ---------------------------------------------------------------- vectors
    .section .text.vectors, "ax"
    .macro ventry target
    .balign 128
    b       \target
    .endm

    .macro save_frame
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #248]
    stp     q0, q1, [sp, #272]
    stp     q2, q3, [sp, #304]
    stp     q4, q5, [sp, #336]
    stp     q6, q7, [sp, #368]
    stp     q8, q9, [sp, #400]
    stp     q10, q11, [sp, #432]
    stp     q12, q13, [sp, #464]
    stp     q14, q15, [sp, #496]
    stp     q16, q17, [sp, #528]
    stp     q18, q19, [sp, #560]
    stp     q20, q21, [sp, #592]
    stp     q22, q23, [sp, #624]
    stp     q24, q25, [sp, #656]
    stp     q26, q27, [sp, #688]
    stp     q28, q29, [sp, #720]
    stp     q30, q31, [sp, #752]
    .endm

    .macro restore_frame
    ldp     x0, x1, [sp, #248]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     q0, q1, [sp, #272]
    ldp     q2, q3, [sp, #304]
    ldp     q4, q5, [sp, #336]
    ldp     q6, q7, [sp, #368]
    ldp     q8, q9, [sp, #400]
    ldp     q10, q11, [sp, #432]
    ldp     q12, q13, [sp, #464]
    ldp     q14, q15, [sp, #496]
    ldp     q16, q17, [sp, #528]
    ldp     q18, q19, [sp, #560]
    ldp     q20, q21, [sp, #592]
    ldp     q22, q23, [sp, #624]
    ldp     q24, q25, [sp, #656]
    ldp     q26, q27, [sp, #688]
    ldp     q28, q29, [sp, #720]
    ldp     q30, q31, [sp, #752]
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret
    .endm

    .balign 2048
    .globl vectors
vectors:
    ventry  unhandled           // current EL, SP0: sync
    ventry  unhandled           // irq
    ventry  unhandled           // fiq
    ventry  unhandled           // serror
    ventry  sync_el1h           // current EL, SPx: sync
    ventry  irq_el1h            // irq
    ventry  unhandled           // fiq
    ventry  unhandled           // serror
    ventry  unhandled           // lower EL aarch64 x4
    ventry  unhandled
    ventry  unhandled
    ventry  unhandled
    ventry  unhandled           // lower EL aarch32 x4
    ventry  unhandled
    ventry  unhandled
    ventry  unhandled

sync_el1h:
    save_frame
    mov     x0, sp
    bl      rust_sync
    restore_frame

irq_el1h:
    save_frame
    mov     x0, sp
    bl      rust_irq
    restore_frame

unhandled:
    mov     x0, #6                      // HVC_UNHANDLED: host dumps the state
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mrs     x3, elr_el1
    hvc     #0
4:  wfe
    b       4b

// ------------------------------------------------- guarded window accesses
// try_load32(addr) -> (x0 = value, x1 = 0) or (x1 = x86 #PF error code):
// the fault handler, finding RESUME set, redirects ELR there — the
// prototype's stand-in for cpu_loop_exit()'s longjmp.
    .section .text, "ax"
    .globl try_load32
try_load32:
    adrp    x3, RESUME
    add     x3, x3, :lo12:RESUME
    adr     x2, 1f
    str     x2, [x3]
    ldr     w0, [x0]
    mov     x1, #0
    str     xzr, [x3]
    ret
1:  str     xzr, [x3]
    ret

    .globl try_store32
try_store32:
    adrp    x3, RESUME
    add     x3, x3, :lo12:RESUME
    adr     x2, 1f
    str     x2, [x3]
    str     w1, [x0]
    mov     x1, #0
    str     xzr, [x3]
    ret
1:  str     xzr, [x3]
    ret

// ---------------------------------------------------------- early tables
    .section .bss.pt, "aw", @nobits
    .balign 4096
    .globl pt_l0
pt_l0:      .skip 4096
    .globl pt_l1
pt_l1:      .skip 4096
    .globl pt_l2_img
pt_l2_img:  .skip 4096
