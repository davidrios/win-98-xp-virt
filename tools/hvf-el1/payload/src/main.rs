//! hvf-el1 payload: the EL1 guest of the feasibility probe (see
//! docs/tracks/m9-tcg-aarch64.md). Runs freestanding under
//! Hypervisor.framework, measures what the "TCG output inside an Arm VM
//! with the x86 page tables mirrored in stage 1" design would pay for
//! each primitive, and reports through the host over HVC.
#![no_std]
#![no_main]

use core::arch::{asm, global_asm};
use core::fmt::Write;
use core::ptr::{read_volatile, write_volatile};

mod mmu;
use mmu::{arena, x86ram, Pool, Space, X86Alloc, WIN0, X86RAM_SIZE, X_A, X_D, X_RW, X_US};

global_asm!(include_str!("start.S"));
global_asm!(include_str!("../../bench.S"));

include!("../../benchlib.rs");

// ---------------------------------------------------------------- layout
const MMIO: u64 = 0xF000_0000;
const PRINTBUF_LEN: usize = 0x10000;
const IDX_N: usize = 1 << 20;
const PTPOOL_SIZE: u64 = 4 << 20;
fn mbox_base() -> u64 { arena() }
fn printbuf() -> u64 { arena() + 0x1000 }
fn jit() -> u64 { arena() + (1 << 20) }
fn ptpool() -> u64 { arena() + (4 << 20) }
fn tlbtab() -> u64 { arena() + (8 << 20) }
fn idx_base() -> u64 { arena() + (9 << 20) }
fn perm_base() -> u64 { arena() + (13 << 20) }

// HVC calls (x0)
const HVC_PRINT: u64 = 1;
const HVC_NOP: u64 = 2;
const HVC_KICK: u64 = 3;
const HVC_IRQ_ACK: u64 = 4;
const HVC_PANIC: u64 = 5;

// mailbox slots (u64)
const MB_KICK_T0: usize = 2;
const MB_KICK_TO_EXIT: usize = 5;
const MB_KICK_TO_ACK: usize = 6;

extern "C" {
    #[link_name = "_bench_chase_direct"]
    fn bench_chase_direct(base: u64, first: u64, n: u64) -> u64;
    #[link_name = "_bench_chase_softmmu"]
    fn bench_chase_softmmu(env: u64, first: u64, n: u64) -> u64;
    #[link_name = "_bench_chase_pinned"]
    fn bench_chase_pinned(env: u64, first: u64, n: u64) -> u64;
    #[link_name = "_bench_sum_direct"]
    fn bench_sum_direct(base: u64, idx: u64, n: u64) -> u64;
    #[link_name = "_bench_sum_softmmu"]
    fn bench_sum_softmmu(env: u64, idx: u64, n: u64) -> u64;
    #[link_name = "_bench_sum_pinned"]
    fn bench_sum_pinned(env: u64, idx: u64, n: u64) -> u64;
    #[link_name = "_bench_touch"]
    fn bench_touch(base: u64, count: u64, stride: u64) -> u64;
    #[link_name = "_bench_movs_softmmu"]
    fn bench_movs_softmmu(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_movs_direct"]
    fn bench_movs_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_movs_fast"]
    fn bench_movs_fast(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_movs_today"]
    fn bench_movs_today(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_movs_today_nomb"]
    fn bench_movs_today_nomb(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_movs_direct_nocc"]
    fn bench_movs_direct_nocc(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_stos_direct"]
    fn bench_stos_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_lods_direct"]
    fn bench_lods_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stenv"]
    fn bench_diag_stenv(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stenv_far"]
    fn bench_diag_stenv_far(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_ldst_env"]
    fn bench_diag_ldst_env(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_ststack"]
    fn bench_diag_ststack(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stwin_ldline"]
    fn bench_diag_stwin_ldline(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stenv_ldbase"]
    fn bench_diag_stenv_ldbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stenv_stbase"]
    fn bench_diag_stenv_stbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    #[link_name = "_bench_diag_stenv_ldstbase"]
    fn bench_diag_stenv_ldstbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
}

#[no_mangle]
pub static mut RESUME: u64 = 0;

// ----------------------------------------------------------------- basics
#[inline(always)]
fn hvc(a0: u64, a1: u64, a2: u64, a3: u64) -> u64 {
    let r: u64;
    unsafe {
        asm!("hvc #0", inout("x0") a0 => r, in("x1") a1, in("x2") a2, in("x3") a3, options(nostack));
    }
    r
}

#[inline(always)]
fn ticks() -> u64 {
    let t: u64;
    unsafe { asm!("isb", "mrs {t}, cntvct_el0", t = out(reg) t, options(nostack, nomem)) }
    t
}

fn cntfrq() -> u64 {
    let t: u64;
    unsafe { asm!("mrs {t}, cntfrq_el0", t = out(reg) t, options(nostack, nomem)) }
    t
}

macro_rules! sysreg {
    ($name:literal) => {{
        let v: u64;
        unsafe { asm!(concat!("mrs {v}, ", $name), v = out(reg) v, options(nostack, nomem)) }
        v
    }};
}

static mut OUT_LEN: usize = 0;
struct Con;
impl Write for Con {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        unsafe {
            let len = OUT_LEN;
            let n = s.len().min(PRINTBUF_LEN - len);
            core::ptr::copy_nonoverlapping(s.as_ptr(), (printbuf() as *mut u8).add(len), n);
            OUT_LEN = len + n;
        }
        Ok(())
    }
}

fn flush() {
    unsafe {
        if OUT_LEN > 0 {
            hvc(HVC_PRINT, printbuf(), OUT_LEN as u64, 0);
            OUT_LEN = 0;
        }
    }
}

macro_rules! prln {
    ($($a:tt)*) => {{ let _ = writeln!(Con, $($a)*); flush(); }};
}

static mut PANICKING: bool = false;

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    unsafe {
        if PANICKING {
            // the report itself faulted: hand the raw state to the host
            asm!("mov x0, #6", "mrs x1, esr_el1", "mrs x2, far_el1", "mrs x3, elr_el1", "hvc #0",
                 out("x0") _, out("x1") _, out("x2") _, out("x3") _, options(nostack));
        }
        PANICKING = true;
        OUT_LEN = 0;
    }
    let _ = write!(Con, "guest panic: {}", info);
    unsafe {
        hvc(HVC_PANIC, printbuf(), OUT_LEN as u64, 0);
    }
    loop {
        unsafe { asm!("wfe") }
    }
}

fn mbox(i: usize) -> u64 {
    unsafe { read_volatile((mbox_base() as *const u64).add(i)) }
}

fn mbox_set(i: usize, v: u64) {
    unsafe { write_volatile((mbox_base() as *mut u64).add(i), v) }
}

fn ns_str(ps: u64) -> (u64, u64) {
    ns(ps)
}

// ------------------------------------------------------------ handlers
#[repr(C)]
pub struct Frame {
    x: [u64; 31],
    elr: u64,
    spsr: u64,
    _pad: u64,
    q: [u128; 32],
}

static mut FAULTS: u64 = 0;
static mut IRQS: u64 = 0;
static mut POOL: Pool = Pool { next: 0, end: 0 };
static mut CUR: *const Space = core::ptr::null();

#[no_mangle]
pub extern "C" fn rust_sync(f: *mut Frame) {
    let esr = sysreg!("esr_el1");
    let far = sysreg!("far_el1");
    let ec = esr >> 26;
    let dfsc = esr & 0x3f;
    let write = esr & (1 << 6) != 0;
    if ec == 0x15 {
        return; // svc: the in-VM trap baseline
    }
    unsafe {
        if ec == 0x25 && (dfsc & 0x3c == 0x04 || dfsc & 0x3c == 0x0c) && mmu::in_window(far) && !CUR.is_null() {
            let pool = &mut *core::ptr::addr_of_mut!(POOL);
            match mmu::handle_fault(&*CUR, pool, far, write) {
                Ok(()) => {
                    FAULTS += 1;
                    return;
                }
                Err(code) => {
                    let resume = read_volatile(core::ptr::addr_of!(RESUME));
                    if resume != 0 {
                        (*f).elr = resume;
                        (*f).x[0] = far;
                        (*f).x[1] = code as u64;
                        return;
                    }
                    panic!("x86 #PF {:#x} at linear {:#x}, no resume point (elr {:#x})", code, far - WIN0, (*f).elr);
                }
            }
        }
        panic!("unhandled sync exception: esr {:#x} far {:#x} elr {:#x}", esr, far, (*f).elr);
    }
}

#[no_mangle]
pub extern "C" fn rust_irq(_f: *mut Frame) {
    let t1 = ticks();
    let t0 = mbox(MB_KICK_T0);
    mbox_set(3, t1.wrapping_sub(t0));
    unsafe {
        IRQS += 1;
    }
    hvc(HVC_IRQ_ACK, 0, 0, 0);
}

// --------------------------------------------------------- experiments
struct Ctx {
    freq: u64,
    alloc: X86Alloc,
    space_a: Space,
    space_b: Space,
}

fn faults() -> u64 {
    unsafe { read_volatile(core::ptr::addr_of!(FAULTS)) }
}

fn exp_idregs() {
    let mmfr0 = sysreg!("id_aa64mmfr0_el1");
    let mmfr1 = sysreg!("id_aa64mmfr1_el1");
    let mmfr2 = sysreg!("id_aa64mmfr2_el1");
    let ctr = sysreg!("ctr_el0");
    let el = sysreg!("currentel") >> 2;
    let midr = sysreg!("midr_el1");
    prln!("id: CurrentEL={} MIDR={:#x} CNTFRQ={} Hz", el, midr, cntfrq());
    prln!(
        "id: ID_AA64MMFR0={:#x} TGran4={} TGran16={} TGran64={} ASIDBits={} PARange={}",
        mmfr0,
        (mmfr0 >> 28) & 0xf,
        (mmfr0 >> 20) & 0xf,
        (mmfr0 >> 24) & 0xf,
        (mmfr0 >> 4) & 0xf,
        mmfr0 & 0xf
    );
    prln!(
        "id: ID_AA64MMFR1={:#x} HAFDBS={} VH={} PAN={} ID_AA64MMFR2={:#x}",
        mmfr1,
        mmfr1 & 0xf,
        (mmfr1 >> 8) & 0xf,
        (mmfr1 >> 20) & 0xf,
        mmfr2
    );
    prln!("id: CTR_EL0={:#x} DIC={} IDC={}", ctr, (ctr >> 29) & 1, (ctr >> 28) & 1);
    prln!("id: SCTLR_EL1={:#x} TCR_EL1={:#x}", sysreg!("sctlr_el1"), sysreg!("tcr_el1"));
}

fn exp_hvc(c: &Ctx) {
    let n = 200_000u64;
    let t0 = ticks();
    for _ in 0..n {
        hvc(HVC_NOP, 0, 0, 0);
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("exit: hvc round trip (guest -> host -> guest): {}.{:02} ns  [{} calls]", a, b, n);
}

fn exp_mmio(c: &Ctx) {
    let n = 100_000u64;
    let mut sum = 0u64;
    let t0 = ticks();
    for _ in 0..n {
        let v: u64;
        unsafe { asm!("ldr {v:w}, [{a}]", v = out(reg) v, a = in(reg) MMIO, options(nostack)) }
        sum = sum.wrapping_add(v);
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("exit: MMIO load (stage-2 fault -> host emulates -> retry): {}.{:02} ns  [{} loads, sum {}]", a, b, n, sum);
}

fn exp_svc(c: &Ctx) {
    let n = 1_000_000u64;
    let t0 = ticks();
    for _ in 0..n {
        unsafe { asm!("svc #0", options(nostack)) }
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("trap: in-VM exception round trip (svc -> EL1 vector -> save/restore -> eret): {}.{:02} ns  [{} traps]", a, b, n);
}

fn exp_stage2(c: &Ctx) {
    // the x86 RAM was pre-faulted by the host process; HVF still populates
    // stage 2 on the guest's first touch (in the kernel, no exit to us)
    let pages = X86RAM_SIZE / 4096;
    let t0 = ticks();
    let x = unsafe { bench_touch(x86ram(), pages, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, pages));
    prln!("stage2: first guest touch of the {} MiB x86 RAM (identity map): {}.{:02} ns/4K page  [HVF's lazy stage-2 fill, once per page; xor {:#x}]", X86RAM_SIZE >> 20, a, b, x);
}

#[inline(never)]
#[no_mangle]
extern "C" fn helper_addx(a: u64, b: u64) -> u64 {
    a.wrapping_mul(3).wrapping_add(b)
}

fn exp_call(c: &Ctx) {
    let n = 20_000_000u64;
    let f: extern "C" fn(u64, u64) -> u64 = helper_addx;
    let fp = unsafe { read_volatile(&f as *const _ as *const usize) };
    let f: extern "C" fn(u64, u64) -> u64 = unsafe { core::mem::transmute(fp) };
    let mut acc = 1u64;
    let t0 = ticks();
    for i in 0..n {
        acc = f(acc, i);
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("call: in-VM helper call (bl to a Rust function): {}.{:02} ns  [{} calls, acc {}]", a, b, n, acc);
}

fn exp_faultfill(c: &Ctx) {
    // linear [0, 32 MiB): first touch faults into the handler and mirrors the page
    let pages = 8192u64;
    let f0 = faults();
    let t0 = ticks();
    unsafe { bench_touch(WIN0, pages, 4096) };
    let dt = ticks() - t0;
    let nf = faults() - f0;
    let (a, b) = ns_str(ps_per(dt, c.freq, pages));
    prln!("fill: first touch of {} pages through the window: {}.{:02} ns/page  [{} faults: abort -> x86 walk -> PTE -> eret]", pages, a, b, nf);

    // the same pages, mirrored already, after a full TLB invalidation: hardware walks only
    unsafe { asm!("dsb ish", "tlbi vmalle1", "dsb ish", "isb", options(nostack)) };
    let f0 = faults();
    let t0 = ticks();
    unsafe { bench_touch(WIN0, pages, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, pages));
    prln!("fill: same {} pages after tlbi vmalle1: {}.{:02} ns/page  [{} faults: TLB misses, stage-1+2 walks]", pages, a, b, faults() - f0);

    let t0 = ticks();
    unsafe { bench_touch(WIN0, 1024, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, 1024));
    prln!("fill: 1024 pages touched again (warm): {}.{:02} ns/page", a, b);
}

fn exp_pf(c: &Ctx) {
    // an unmapped linear page: the handler finds no PTE and lands on the resume point
    let bad = WIN0 + 0xFF00_0000;
    let (v, code) = try32(bad);
    prln!("pf: read of unmapped linear 0xff000000 -> x86 #PF error code {} (value {:#x})", code, v);
    let n = 20_000u64;
    let t0 = ticks();
    for _ in 0..n {
        try32(bad);
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("pf: #PF round trip (abort -> walk -> resume, the in-VM cpu_loop_exit): {}.{:02} ns", a, b);
}

fn try32(addr: u64) -> (u64, u64) {
    let v: u64;
    let code: u64;
    unsafe {
        asm!("bl try_load32", inout("x0") addr => v, out("x1") code, out("x2") _, out("x3") _, out("x30") _, options(nostack));
    }
    (v, code)
}

fn trystore32(addr: u64, val: u64) -> u64 {
    let code: u64;
    unsafe {
        asm!("bl try_store32", inout("x0") addr => _, inout("x1") val => code, out("x2") _, out("x3") _, out("x30") _, options(nostack));
    }
    code
}

fn exp_dirty(c: &mut Ctx) {
    // page 9000: RW, D clear -> mirrored RO on read; the first write faults, sets D, remaps RW
    let lin = 9000u32 << 12;
    mmu::x86_map(&mut c.alloc, c.space_a.cr3, lin, 9000, X_RW | X_US);
    let va = WIN0 + lin as u64;
    let (_, code) = try32(va);
    let pte_after_read = mmu::x86_pte(c.space_a.cr3, lin);
    let f0 = faults();
    let t0 = ticks();
    let scode = trystore32(va, 0x1234_5678);
    let dt = ticks() - t0;
    let pte_after_write = mmu::x86_pte(c.space_a.cr3, lin);
    let (a, b) = ns_str(ps_per(dt, c.freq, 1));
    prln!(
        "dirty: read code {} (PTE {:#x}: A set, D clear, mirrored RO); write code {} in {}.{:02} ns, {} fault(s), PTE {:#x} (D set, remapped RW); value {:#x}",
        code,
        pte_after_read,
        scode,
        a,
        b,
        faults() - f0,
        pte_after_write,
        unsafe { read_volatile((x86ram() + ((9000u64) << 12)) as *const u32) }
    );
    // page 9001: read-only -> the write is an x86 #PF with code 3
    let lin = 9001u32 << 12;
    mmu::x86_map(&mut c.alloc, c.space_a.cr3, lin, 9001, X_US | X_A | X_D);
    let va = WIN0 + lin as u64;
    let (_, rcode) = try32(va);
    let wcode = trystore32(va, 1);
    prln!("dirty: read-only page: read code {}, write code {} (expect 0 and 3)", rcode, wcode);
    // 1024 clean pages: read them all (mirrored RO), then write them all
    let n = 512u32; // linear pages 20000.., x86 phys pages 11700.. (free RAM above the tables)
    for i in 0..n {
        mmu::x86_map(&mut c.alloc, c.space_a.cr3, (20000 + i) << 12, 11700 + i, X_RW | X_US);
    }
    unsafe { bench_touch(WIN0 + (20000u64 << 12), n as u64, 4096) };
    let f0 = faults();
    let t0 = ticks();
    for i in 0..n {
        unsafe { write_volatile((WIN0 + (((20000 + i) as u64) << 12)) as *mut u32, i) };
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n as u64));
    prln!("dirty: first write to {} clean pages (permission fault -> D set -> remap RW -> tlbi vae1is + dsb ish): {}.{:02} ns/page, {} faults", n, a, b, faults() - f0);
    // the same with the non-broadcast invalidate
    for i in 0..n {
        mmu::x86_map(&mut c.alloc, c.space_a.cr3, (21000 + i) << 12, 11700 + i, X_RW | X_US);
    }
    unsafe { bench_touch(WIN0 + (21000u64 << 12), n as u64, 4096) };
    unsafe { write_volatile(core::ptr::addr_of_mut!(mmu::TLBI_LOCAL), true) };
    let f0 = faults();
    let t0 = ticks();
    for i in 0..n {
        unsafe { write_volatile((WIN0 + (((21000 + i) as u64) << 12)) as *mut u32, i) };
    }
    let dt = ticks() - t0;
    unsafe { write_volatile(core::ptr::addr_of_mut!(mmu::TLBI_LOCAL), false) };
    let (a, b) = ns_str(ps_per(dt, c.freq, n as u64));
    prln!("dirty: the same with tlbi vae1 + dsb nsh (no broadcast): {}.{:02} ns/page, {} faults", a, b, faults() - f0);
}

fn bench_set(c: &Ctx, name: &str, pages: usize, rng: &mut Rng) {
    let mem = x86ram() as *mut u8; // the setup writes through the identity mapping
    let perm = perm_base() as *mut u32;
    let idx = idx_base() as *mut u32;
    let entries = tlb_entries_for(pages);
    let env = tlbtab() as *mut u64;
    let table = (tlbtab() + 64) as *mut u64;
    let first = unsafe { build_chase(mem, pages, perm, rng) } as u64;
    unsafe {
        build_idx(idx, IDX_N, pages, rng);
        build_tlb(env, table, entries, pages, WIN0);
    }
    let steps = 2_000_000u64;
    let passes = 4u64;
    let f0 = faults();

    // warm
    unsafe {
        bench_chase_direct(WIN0, first, steps / 4);
    }
    let t0 = ticks();
    let r1 = unsafe { bench_chase_direct(WIN0, first, steps) };
    let t1 = ticks();
    let r2 = unsafe { bench_chase_softmmu(env as u64, first, steps) };
    let t2 = ticks();
    let r3 = unsafe { bench_chase_pinned(env as u64, first, steps) };
    let t3 = ticks();
    if r1 != r2 || r2 != r3 {
        panic!("chase results differ: {:#x} {:#x} {:#x}", r1, r2, r3);
    }
    let (d1, d2) = ns_str(ps_per(t1 - t0, c.freq, steps));
    let (s1, s2) = ns_str(ps_per(t2 - t1, c.freq, steps));
    let (p1, p2) = ns_str(ps_per(t3 - t2, c.freq, steps));
    prln!(
        "load: {:>7} chase  direct {}.{:02} ns  softmmu {}.{:02} ns  pinned {}.{:02} ns   [{} steps, TLB {} entries]",
        name, d1, d2, s1, s2, p1, p2, steps, entries
    );

    let mut sums = [0u64; 3];
    let t0 = ticks();
    for _ in 0..passes {
        sums[0] = sums[0].wrapping_add(unsafe { bench_sum_direct(WIN0, idx_base(), IDX_N as u64) });
    }
    let t1 = ticks();
    for _ in 0..passes {
        sums[1] = sums[1].wrapping_add(unsafe { bench_sum_softmmu(env as u64, idx_base(), IDX_N as u64) });
    }
    let t2 = ticks();
    for _ in 0..passes {
        sums[2] = sums[2].wrapping_add(unsafe { bench_sum_pinned(env as u64, idx_base(), IDX_N as u64) });
    }
    let t3 = ticks();
    if sums[0] != sums[1] || sums[1] != sums[2] {
        panic!("sum results differ: {:#x} {:#x} {:#x}", sums[0], sums[1], sums[2]);
    }
    let n = passes * IDX_N as u64;
    let (d1, d2) = ns_str(ps_per(t1 - t0, c.freq, n));
    let (s1, s2) = ns_str(ps_per(t2 - t1, c.freq, n));
    let (p1, p2) = ns_str(ps_per(t3 - t2, c.freq, n));
    prln!(
        "load: {:>7} indep  direct {}.{:02} ns  softmmu {}.{:02} ns  pinned {}.{:02} ns   [{} loads, {} faults during the set]",
        name, d1, d2, s1, s2, p1, p2, n, faults() - f0
    );
}

/// The blit loop of a 2D game (`rep movsd` per 640-byte row) five ways:
/// today's TCG loop exactly (with and without its barriers), the same
/// loop with pinned guest registers and the softmmu chain, with pinned
/// registers and direct window accesses (the VM design), and a per-row
/// probe + vector copy (a REP MOVS fast path, no VM needed).  Buffers of
/// three sizes: L1, L2 and beyond.
fn exp_movs(c: &Ctx) {
    let env = tlbtab() as *mut u64;
    let table = (tlbtab() + 0x1000) as *mut u64;
    let w = 160u64;
    for (name, rows) in [("32 KiB", 51u64), ("512 KiB", 819), ("8 MiB", 13107)] {
        let bytes = rows * w * 4;
        let pages = ((2 * bytes + 4095) / 4096) as usize;
        let passes = ((96u64 << 20) / bytes).max(1);
        let n = passes * rows * w;
        let f = |x: u64| ns_str(ps_per(x, c.freq, n));
        // env (the emulated CPU state the loop stores to) in the block-mapped
        // identity region, then inside the 4 KiB-page window like the guest data
        let ewin = (WIN0 + (24 << 20)) as *mut u64;   // the window maps [0, 32 MiB); the buffers end at 16 MiB
        for (en, e, tb) in [("env identity", env, table), ("env in window", ewin, (ewin as u64 + 0x1000) as *mut u64)] {
            unsafe { build_tlb(e, tb, 4096, pages, WIN0) };
            let (t, chk) = unsafe {
                run_movs(WIN0, e as u64, rows, w, passes, ticks,
                         [bench_movs_today as MovsFn, bench_movs_today_nomb, bench_movs_softmmu, bench_movs_direct, bench_movs_fast])
            };
            if chk.iter().any(|&x| x != chk[0]) {
                prln!("movs: {:>7} {}: kernels disagree or a probe refused: {:x?}", name, en, chk);
                continue;
            }
            let (a1, a2) = f(t[0]); let (b1, b2) = f(t[1]); let (c1, c2) = f(t[2]); let (d1, d2) = f(t[3]); let (e1, e2) = f(t[4]);
            prln!(
                "movs: {:>7} x2, rows of {} B, {}  today {}.{:02} ns/dword  no-mb {}.{:02}  pinned+softmmu {}.{:02}  pinned+direct {}.{:02}  fast {}.{:02}   [{} rows, {} passes]",
                name, w * 4, en, a1, a2, b1, b2, c1, c2, d1, d2, e1, e2, rows, passes
            );
        }
        unsafe { build_tlb(env, table, 4096, pages, WIN0) };
        let (t, _) = unsafe {
            run_movs(WIN0, env as u64, rows, w, passes, ticks, [bench_movs_direct_nocc as MovsFn, bench_stos_direct, bench_lods_direct])
        };
        let (a1, a2) = f(t[0]); let (b1, b2) = f(t[1]); let (c1, c2) = f(t[2]);
        prln!("movs: {:>7} diag  direct-no-cc {}.{:02}  stores-only {}.{:02}  loads-only {}.{:02}", name, a1, a2, b1, b2, c1, c2);
        let (t, _) = unsafe {
            run_movs(WIN0, env as u64, rows, w, passes, ticks, [bench_diag_stenv as MovsFn, bench_diag_stenv_far, bench_diag_ldst_env, bench_diag_ststack, bench_diag_stwin_ldline])
        };
        let (a1, a2) = f(t[0]); let (b1, b2) = f(t[1]); let (c1, c2) = f(t[2]); let (d1, d2) = f(t[3]); let (e1, e2) = f(t[4]);
        for (bn, b) in [("window", WIN0), ("identity", x86ram())] {
            let (t, _) = unsafe {
                run_movs(b, env as u64, rows, w, passes, ticks, [bench_diag_stenv_ldbase as MovsFn, bench_diag_stenv_stbase, bench_diag_stenv_ldstbase])
            };
            let (a1, a2) = f(t[0]); let (b1, b2) = f(t[1]); let (c1, c2) = f(t[2]);
            prln!("movs: {:>7} diag3 via {}: st-env+ld {}.{:02}  st-env+st {}.{:02}  st-env+ld+st {}.{:02}", name, bn, a1, a2, b1, b2, c1, c2);
        }
        prln!("movs: {:>7} diag2 st-env {}.{:02}  st-env-far {}.{:02}  ld+st-env {}.{:02}  st-stack {}.{:02}  st+ld-window-line {}.{:02}", name, a1, a2, b1, b2, c1, c2, d1, d2, e1, e2);
    }
}

fn exp_loads(c: &Ctx) {
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    bench_set(c, "64 KiB", 16, &mut rng);
    bench_set(c, "4 MiB", 1024, &mut rng);
    bench_set(c, "8 MiB", 2048, &mut rng);
    bench_set(c, "16 MiB", 4096, &mut rng);
    bench_set(c, "32 MiB", 8192, &mut rng);
}

fn icache_sync(addr: u64) {
    unsafe {
        asm!(
            "dc cvau, {a}", "dsb ish", "ic ivau, {a}", "dsb ish", "isb",
            a = in(reg) addr, options(nostack)
        )
    }
}

fn exp_jit(c: &Ctx) {
    // the host wrote  mov w0, #42 ; add w0, w0, w1 ; ret  at jit() before starting the VM
    icache_sync(jit());
    let f: extern "C" fn(u64, u64) -> u64 = unsafe { core::mem::transmute(jit() as usize) };
    let r = f(0, 100);
    prln!("jit: code written by the host process executes in the VM: f(100) = {} (expect 142)", r);
    // self-patching: rewrite the immediate, sync, call — no W^X toggle exists here
    let n = 100_000u64;
    let mut bad = 0u64;
    let t0 = ticks();
    for i in 0..n {
        let imm = (i & 0xffff) as u32;
        unsafe { write_volatile(jit() as *mut u32, 0x5280_0000 | (imm << 5)) };
        icache_sync(jit());
        if f(0, 1) != imm as u64 + 1 {
            bad += 1;
        }
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("jit: patch + dc cvau/ic ivau/isb + call: {}.{:02} ns  [{} rounds, {} wrong]", a, b, n, bad);
}

fn exp_asid(c: &mut Ctx) {
    // A: linear pages 0..1023 -> phys 0..1023 (already mapped); B: -> phys 10240..11263
    for p in 0..1024u32 {
        mmu::x86_map(&mut c.alloc, c.space_b.cr3, p << 12, 10240 + p, X_RW | X_US | X_A | X_D);
        unsafe {
            write_volatile((x86ram() + ((p as u64) << 12)) as *mut u32, 0xA000_0000 | p);
            write_volatile((x86ram() + (((10240 + p) as u64) << 12)) as *mut u32, 0xB000_0000 | p);
        }
    }
    // switch to B: the first touch of its pages faults them in
    c.space_b.activate();
    unsafe { CUR = &c.space_b };
    let f0 = faults();
    let t0 = ticks();
    let x = unsafe { bench_touch(WIN0, 1024, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, 1024));
    prln!("asid: B first touch, 1024 pages: {}.{:02} ns/page, {} faults (xor {:#x})", a, b, faults() - f0, x);

    // the switch itself
    let n = 100_000u64;
    let t0 = ticks();
    for _ in 0..n / 2 {
        c.space_a.activate();
        c.space_b.activate();
    }
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, n));
    prln!("asid: CR3 switch (msr ttbr0_el1 + isb, tables kept): {}.{:02} ns", a, b);

    // back on A: its pages are still mirrored, no faults
    c.space_a.activate();
    unsafe { CUR = &c.space_a };
    let f0 = faults();
    let t0 = ticks();
    let x = unsafe { bench_touch(WIN0, 1024, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, 1024));
    prln!("asid: A after the switches, 1024 pages: {}.{:02} ns/page, {} faults (xor {:#x})", a, b, faults() - f0, x);

    // the flush model (QEMU today: every CR3 write empties the TLB): drop and refault
    let t0 = ticks();
    c.space_a.clear_window();
    let x = unsafe { bench_touch(WIN0, 1024, 4096) };
    let dt = ticks() - t0;
    let (a, b) = ns_str(ps_per(dt, c.freq, 1024));
    prln!("asid: A flushed and refaulted, 1024 pages: {}.{:02} ns/page (xor {:#x})", a, b, x);

    // correctness: each space sees its own pages
    let mut ok = true;
    for p in [0u32, 1, 511, 1023] {
        let va = WIN0 + ((p as u64) << 12);
        c.space_a.activate();
        unsafe { CUR = &c.space_a };
        let va_a = unsafe { read_volatile(va as *const u32) };
        c.space_b.activate();
        unsafe { CUR = &c.space_b };
        let va_b = unsafe { read_volatile(va as *const u32) };
        if va_a != 0xA000_0000 | p || va_b != 0xB000_0000 | p {
            ok = false;
            prln!("asid: page {} reads {:#x} under A, {:#x} under B", p, va_a, va_b);
        }
    }
    c.space_a.activate();
    unsafe { CUR = &c.space_a };
    prln!("asid: A and B see their own pages through the same window: {}", if ok { "ok" } else { "FAIL" });
}

fn exp_irq(c: &Ctx) {
    let trials = 20u64;
    let mut lat = [0u64; 20];
    let mut kick = [0u64; 20];
    let mut ack = [0u64; 20];
    for t in 0..trials as usize {
        let before = unsafe { read_volatile(core::ptr::addr_of!(IRQS)) };
        mbox_set(3, 0);
        mbox_set(MB_KICK_TO_EXIT, 0);
        mbox_set(MB_KICK_TO_ACK, 0);
        hvc(HVC_KICK, 300, ticks(), 0); // the host kicks in 300 us from another thread; x2 calibrates the clocks
        let deadline = ticks() + c.freq; // 1 s
        unsafe { asm!("msr daifclr, #2", options(nostack)) };
        while unsafe { read_volatile(core::ptr::addr_of!(IRQS)) } == before {
            if ticks() > deadline {
                break;
            }
        }
        unsafe { asm!("msr daifset, #2", options(nostack)) };
        lat[t] = mbox(3);
        kick[t] = mbox(MB_KICK_TO_EXIT);
        ack[t] = mbox(MB_KICK_TO_ACK);
        if lat[t] == 0 {
            prln!("irq: trial {} timed out", t);
        }
    }
    let to_ns = |v: u64| ps_per(v, c.freq, 1) / 1000;
    sort(&mut lat);
    sort(&mut kick);
    sort(&mut ack);
    prln!(
        "irq: host thread kick -> guest IRQ handler: min {} / median {} / max {} ns  [{} trials; guest clock, calibrated at the kick request]",
        to_ns(lat[0]),
        to_ns(lat[trials as usize / 2]),
        to_ns(lat[trials as usize - 1]),
        trials
    );
    prln!(
        "irq: of which hv_vcpus_exit -> hv_vcpu_run returns (host side): min {} / median {} ns; kick -> the handler's ack HVC seen by the host: min {} / median {} ns",
        to_ns(kick[0]),
        to_ns(kick[trials as usize / 2]),
        to_ns(ack[0]),
        to_ns(ack[trials as usize / 2])
    );
}

fn sort(v: &mut [u64]) {
    for i in 1..v.len() {
        let mut j = i;
        while j > 0 && v[j - 1] > v[j] {
            v.swap(j - 1, j);
            j -= 1;
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_main() {
    let freq = cntfrq();
    prln!("guest: up at EL1, MMU on, {} MHz counter, arena at {:#x} (host VA == IPA == guest VA)", freq / 1_000_000, arena());
    exp_idregs();

    // the x86 side: CR3 A maps linear [0, 40 MiB) identity, RW and dirty
    let mut alloc = X86Alloc { next_page: 11264 }; // tables from x86 phys 44 MiB
    let cr3_a = alloc.page() << 12;
    let cr3_b = alloc.page() << 12;
    for p in 0..10240u32 {
        mmu::x86_map(&mut alloc, cr3_a, p << 12, p, X_RW | X_US | X_A | X_D);
    }
    let pool = unsafe { &mut *core::ptr::addr_of_mut!(POOL) };
    pool.next = ptpool();
    pool.end = ptpool() + PTPOOL_SIZE;
    let space_a = Space::new(pool, 1, cr3_a);
    let space_b = Space::new(pool, 2, cr3_b);
    let mut c = Ctx { freq, alloc, space_a, space_b };
    c.space_a.activate();
    unsafe { CUR = &c.space_a };
    prln!("guest: x86 tables built (CR3 A {:#x}, CR3 B {:#x}), stage-1 roots with ASIDs 1 and 2", cr3_a, cr3_b);

    exp_hvc(&c);
    exp_mmio(&c);
    exp_svc(&c);
    exp_call(&c);
    exp_stage2(&c);
    exp_faultfill(&c);
    exp_pf(&c);
    exp_dirty(&mut c);
    exp_loads(&c);
    exp_movs(&c);
    exp_jit(&c);
    exp_asid(&mut c);
    exp_irq(&c);
    prln!("guest: done, {} window faults handled in the VM, {} IRQs, tables used {} KiB", faults(), unsafe { IRQS }, (pool.next - ptpool()) / 1024);
}
