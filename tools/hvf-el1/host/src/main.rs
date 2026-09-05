//! hvf-el1 host: creates a Hypervisor.framework VM on Apple Silicon, maps
//! the payload image and a shared arena into it, runs the EL1 guest and
//! services its exits (HVC calls, the MMIO probe, the kick test), then
//! runs the same load kernels natively for the baseline. See
//! docs/tracks/m9-tcg-aarch64.md for what the numbers mean.

use std::arch::global_asm;
use std::ffi::c_void;
use std::io::Write;
use std::ptr::{read_volatile, write_volatile};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

global_asm!(include_str!("../../bench.S"));
include!("../../benchlib.rs");

// ------------------------------------------------- Hypervisor.framework
type HvReturn = i32;
type HvVcpu = u64;

#[repr(C)]
#[derive(Clone, Copy)]
struct HvVcpuExitException {
    syndrome: u64,
    virtual_address: u64,
    physical_address: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct HvVcpuExit {
    reason: u32,
    exception: HvVcpuExitException,
}

const HV_MEMORY_READ: u64 = 1;
const HV_MEMORY_WRITE: u64 = 2;
const HV_MEMORY_EXEC: u64 = 4;
const HV_EXIT_REASON_CANCELED: u32 = 0;
const HV_EXIT_REASON_EXCEPTION: u32 = 1;
const HV_EXIT_REASON_VTIMER_ACTIVATED: u32 = 2;
const HV_REG_X0: u32 = 0;
const HV_REG_PC: u32 = 31;
const HV_REG_CPSR: u32 = 34;
const HV_INTERRUPT_TYPE_IRQ: u32 = 0;
const HV_SYS_REG_SP_EL1: u16 = 0xe208;
const HV_SYS_REG_ESR_EL1: u16 = 0xc290;
const HV_SYS_REG_FAR_EL1: u16 = 0xc300;
const HV_SYS_REG_ELR_EL1: u16 = 0xc201;
const HV_SYS_REG_SCTLR_EL1: u16 = 0xc080;
const HV_SYS_REG_TTBR0_EL1: u16 = 0xc100;

#[link(name = "Hypervisor", kind = "framework")]
extern "C" {
    fn hv_vm_create(config: *mut c_void) -> HvReturn;
    fn hv_vm_destroy() -> HvReturn;
    fn hv_vm_map(addr: *mut c_void, ipa: u64, size: usize, flags: u64) -> HvReturn;
    fn hv_vm_config_get_max_ipa_size(bits: *mut u32) -> HvReturn;
    fn hv_vm_config_get_default_ipa_size(bits: *mut u32) -> HvReturn;
    fn hv_vcpu_create(vcpu: *mut HvVcpu, exit: *mut *mut HvVcpuExit, config: *mut c_void) -> HvReturn;
    fn hv_vcpu_destroy(vcpu: HvVcpu) -> HvReturn;
    fn hv_vcpu_run(vcpu: HvVcpu) -> HvReturn;
    fn hv_vcpu_get_reg(vcpu: HvVcpu, reg: u32, value: *mut u64) -> HvReturn;
    fn hv_vcpu_set_reg(vcpu: HvVcpu, reg: u32, value: u64) -> HvReturn;
    fn hv_vcpu_get_sys_reg(vcpu: HvVcpu, reg: u16, value: *mut u64) -> HvReturn;
    fn hv_vcpu_set_pending_interrupt(vcpu: HvVcpu, kind: u32, pending: bool) -> HvReturn;
    fn hv_vcpu_set_vtimer_mask(vcpu: HvVcpu, masked: bool) -> HvReturn;
    fn hv_vcpu_set_vtimer_offset(vcpu: HvVcpu, offset: u64) -> HvReturn;
    fn hv_vcpus_exit(vcpus: *const HvVcpu, count: u32) -> HvReturn;
    fn sys_dcache_flush(start: *mut c_void, len: usize);
    fn mach_vm_allocate(task: u32, address: *mut u64, size: u64, flags: i32) -> i32;
    static mach_task_self_: u32;
    fn mach_timebase_info(info: *mut [u32; 2]) -> i32;
}

extern "C" {
    fn bench_chase_direct(base: u64, first: u64, n: u64) -> u64;
    fn bench_chase_softmmu(env: u64, first: u64, n: u64) -> u64;
    fn bench_chase_pinned(env: u64, first: u64, n: u64) -> u64;
    fn bench_sum_direct(base: u64, idx: u64, n: u64) -> u64;
    fn bench_sum_softmmu(env: u64, idx: u64, n: u64) -> u64;
    fn bench_sum_pinned(env: u64, idx: u64, n: u64) -> u64;
    fn bench_movs_softmmu(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_movs_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_movs_fast(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_movs_today(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_movs_today_nomb(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_movs_direct_nocc(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_stos_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_lods_direct(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stenv(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stenv_far(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_ldst_env(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_ststack(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stwin_ldline(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stenv_ldbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stenv_stbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn bench_diag_stenv_ldstbase(base: u64, src: u64, dst: u64, rows: u64, w: u64, env: u64) -> u64;
    fn memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8;
}

fn check(what: &str, r: HvReturn) {
    if r != 0 {
        eprintln!("hvf-el1: {what} failed: {r:#x}");
        std::process::exit(2);
    }
}

fn reg(vcpu: HvVcpu, r: u32) -> u64 {
    let mut v = 0;
    check("hv_vcpu_get_reg", unsafe { hv_vcpu_get_reg(vcpu, r, &mut v) });
    v
}

fn sysreg(vcpu: HvVcpu, r: u16) -> u64 {
    let mut v = 0;
    unsafe { hv_vcpu_get_sys_reg(vcpu, r, &mut v) };
    v
}

#[inline(always)]
fn ticks() -> u64 {
    let t: u64;
    unsafe { std::arch::asm!("isb", "mrs {t}, cntvct_el0", t = out(reg) t, options(nostack, nomem)) }
    t
}

fn counter_hz() -> u64 {
    let mut tb = [0u32; 2];
    unsafe { mach_timebase_info(&mut tb) };
    // mach_absolute_time ticks at the counter's rate: ns = ticks * numer / denom
    1_000_000_000u64 * tb[1] as u64 / tb[0] as u64
}

// --------------------------------------------------------------- layout
const IMG_BASE: u64 = 0x4000_0000;
const IMG_SIZE: usize = 4 << 20;
// the arena: host VA == IPA == guest VA, 1 GiB aligned, under the 36-bit IPA limit
const ARENA_CANDIDATES: [u64; 10] = [0x6_0000_0000, 0x7_0000_0000, 0x8_0000_0000, 0x9_0000_0000, 0xa_0000_0000, 0xb_0000_0000, 0xc_0000_0000, 0xd_0000_0000, 0xe_0000_0000, 0xf_0000_0000];
const ARENA_SIZE: usize = 64 << 20;
const MMIO_BASE: u64 = 0xF000_0000;
const MMIO_SIZE: u64 = 0x1000_0000;
const JIT_OFF: usize = 1 << 20;
const MB_KICK_T0: usize = 2;
const MB_KICK_TO_EXIT: usize = 5;
const MB_KICK_TO_ACK: usize = 6;

const HVC_EXIT: u64 = 0;
const HVC_PRINT: u64 = 1;
const HVC_NOP: u64 = 2;
const HVC_KICK: u64 = 3;
const HVC_IRQ_ACK: u64 = 4;
const HVC_PANIC: u64 = 5;
const HVC_UNHANDLED: u64 = 6;

/// Zeroed anonymous memory at exactly `addr` (VM_FLAGS_FIXED fails, rather
/// than replacing, when the range is in use), or None.
fn alloc_fixed(addr: u64, size: usize) -> Option<*mut u8> {
    let mut a = addr;
    let kr = unsafe { mach_vm_allocate(mach_task_self_, &mut a, size as u64, 0 /* VM_FLAGS_FIXED */) };
    if kr == 0 && a == addr {
        Some(a as *mut u8)
    } else {
        None
    }
}

fn mmap_zero(size: usize) -> *mut u8 {
    let p = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANON,
            -1,
            0,
        )
    };
    if p == libc::MAP_FAILED {
        eprintln!("hvf-el1: mmap failed");
        std::process::exit(2);
    }
    p as *mut u8
}

fn dump(vcpu: HvVcpu, exit: &HvVcpuExit) {
    eprintln!(
        "  exit reason {} syndrome {:#x} (EC {:#x}) VA {:#x} IPA {:#x}",
        exit.reason,
        exit.exception.syndrome,
        exit.exception.syndrome >> 26,
        exit.exception.virtual_address,
        exit.exception.physical_address
    );
    eprintln!(
        "  PC {:#x} CPSR {:#x} SP_EL1 {:#x} ELR_EL1 {:#x} ESR_EL1 {:#x} FAR_EL1 {:#x} SCTLR {:#x} TTBR0 {:#x}",
        reg(vcpu, HV_REG_PC),
        reg(vcpu, HV_REG_CPSR),
        sysreg(vcpu, HV_SYS_REG_SP_EL1),
        sysreg(vcpu, HV_SYS_REG_ELR_EL1),
        sysreg(vcpu, HV_SYS_REG_ESR_EL1),
        sysreg(vcpu, HV_SYS_REG_FAR_EL1),
        sysreg(vcpu, HV_SYS_REG_SCTLR_EL1),
        sysreg(vcpu, HV_SYS_REG_TTBR0_EL1)
    );
    for i in (0..31).step_by(4) {
        let mut line = String::new();
        for r in i..(i + 4).min(31) {
            line += &format!("  x{r:<2} {:#018x}", reg(vcpu, HV_REG_X0 + r));
        }
        eprintln!("{line}");
    }
}

fn run_guest(payload: &[u8]) -> bool {
    let img = mmap_zero(IMG_SIZE);
    let Some(arena) = ARENA_CANDIDATES.iter().find_map(|&a| alloc_fixed(a, ARENA_SIZE)) else {
        eprintln!("hvf-el1: no 1 GiB-aligned slot free for the arena at any of {ARENA_CANDIDATES:#x?}");
        return false;
    };
    let arena_base = arena as u64;
    if payload.len() > IMG_SIZE {
        eprintln!("hvf-el1: payload too big");
        return false;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(payload.as_ptr(), img, payload.len());
        // pre-fault the arena so the guest's page-fault numbers are stage-1 only
        for off in (0..ARENA_SIZE).step_by(4096) {
            write_volatile(arena.add(off), 0);
        }
        // the JIT probe: mov w0, #42 ; add w0, w0, w1 ; ret
        let jit = arena.add(JIT_OFF) as *mut u32;
        write_volatile(jit, 0x5280_0540);
        write_volatile(jit.add(1), 0x0b01_0000);
        write_volatile(jit.add(2), 0xd65f_03c0);
        sys_dcache_flush(img as *mut c_void, IMG_SIZE);
        sys_dcache_flush(arena as *mut c_void, ARENA_SIZE);
    }

    let mut max_ipa = 0u32;
    let mut def_ipa = 0u32;
    unsafe {
        hv_vm_config_get_max_ipa_size(&mut max_ipa);
        hv_vm_config_get_default_ipa_size(&mut def_ipa);
    }
    check("hv_vm_create", unsafe { hv_vm_create(std::ptr::null_mut()) });
    println!("host: VM created (IPA size default {def_ipa} / max {max_ipa} bits); image {} KiB at {IMG_BASE:#x}, arena {} MiB at {arena_base:#x} (host VA == IPA), MMIO probe at {MMIO_BASE:#x} unmapped",
        payload.len() / 1024, ARENA_SIZE >> 20);
    let rwx = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    check("hv_vm_map image", unsafe { hv_vm_map(img as *mut c_void, IMG_BASE, IMG_SIZE, rwx) });
    check("hv_vm_map arena", unsafe { hv_vm_map(arena as *mut c_void, arena_base, ARENA_SIZE, rwx) });

    let mut vcpu: HvVcpu = 0;
    let mut exit: *mut HvVcpuExit = std::ptr::null_mut();
    check("hv_vcpu_create", unsafe { hv_vcpu_create(&mut vcpu, &mut exit, std::ptr::null_mut()) });
    check("set PC", unsafe { hv_vcpu_set_reg(vcpu, HV_REG_PC, IMG_BASE) });
    check("set X0", unsafe { hv_vcpu_set_reg(vcpu, HV_REG_X0, arena_base) });
    check("set CPSR", unsafe { hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5) }); // EL1h, DAIF masked
    check("vtimer offset", unsafe { hv_vcpu_set_vtimer_offset(vcpu, 0) });

    let mbox = arena as *mut u64;
    let irq_pending = Arc::new(AtomicBool::new(false));
    let kick_t0 = Arc::new(AtomicU64::new(0));
    let mut ok = false;
    let mut out = std::io::stdout().lock();
    let t_start = ticks();
    let mut exits = 0u64;
    loop {
        if irq_pending.load(Ordering::Acquire) {
            unsafe { hv_vcpu_set_pending_interrupt(vcpu, HV_INTERRUPT_TYPE_IRQ, true) };
        }
        let r = unsafe { hv_vcpu_run(vcpu) };
        exits += 1;
        if r != 0 {
            eprintln!("hvf-el1: hv_vcpu_run failed: {r:#x}");
            break;
        }
        let ex = unsafe { *exit };
        match ex.reason {
            HV_EXIT_REASON_CANCELED => {
                // the kick thread asked for an exit: assert the IRQ from now on
                let now = ticks();
                unsafe { write_volatile(mbox.add(MB_KICK_TO_EXIT), now.wrapping_sub(kick_t0.load(Ordering::Acquire))) };
                irq_pending.store(true, Ordering::Release);
            }
            HV_EXIT_REASON_VTIMER_ACTIVATED => unsafe {
                hv_vcpu_set_vtimer_mask(vcpu, false);
            },
            HV_EXIT_REASON_EXCEPTION => {
                let syn = ex.exception.syndrome;
                let ec = syn >> 26;
                match ec {
                    0x16 => {
                        // HVC: PC already points past it
                        let x0 = reg(vcpu, HV_REG_X0);
                        match x0 {
                            HVC_NOP => {}
                            HVC_PRINT | HVC_PANIC => {
                                let ptr = reg(vcpu, HV_REG_X0 + 1);
                                let len = reg(vcpu, HV_REG_X0 + 2) as usize;
                                if ptr >= arena_base && ptr + len as u64 <= arena_base + ARENA_SIZE as u64 {
                                    let s = unsafe { std::slice::from_raw_parts(ptr as *const u8, len) };
                                    out.write_all(s).unwrap();
                                    out.flush().unwrap();
                                }
                                if x0 == HVC_PANIC {
                                    eprintln!();
                                    dump(vcpu, &ex);
                                    break;
                                }
                            }
                            HVC_KICK => {
                                let delay_us = reg(vcpu, HV_REG_X0 + 1);
                                // host counter - guest counter, measured at this exit
                                let delta = ticks().wrapping_sub(reg(vcpu, HV_REG_X0 + 2));
                                let t0 = kick_t0.clone();
                                let mb = mbox as usize;
                                std::thread::spawn(move || {
                                    std::thread::sleep(Duration::from_micros(delay_us));
                                    let now = ticks();
                                    t0.store(now, Ordering::Release);
                                    unsafe { write_volatile((mb as *mut u64).add(MB_KICK_T0), now.wrapping_sub(delta)) };
                                    unsafe { hv_vcpus_exit(&vcpu, 1) };
                                });
                            }
                            HVC_IRQ_ACK => {
                                let now = ticks();
                                unsafe { write_volatile(mbox.add(MB_KICK_TO_ACK), now.wrapping_sub(kick_t0.load(Ordering::Acquire))) };
                                irq_pending.store(false, Ordering::Release);
                            }
                            HVC_UNHANDLED => {
                                eprintln!(
                                    "hvf-el1: guest took an unhandled exception: ESR_EL1 {:#x} FAR_EL1 {:#x} ELR_EL1 {:#x}",
                                    reg(vcpu, HV_REG_X0 + 1),
                                    reg(vcpu, HV_REG_X0 + 2),
                                    reg(vcpu, HV_REG_X0 + 3)
                                );
                                dump(vcpu, &ex);
                                break;
                            }
                            HVC_EXIT => {
                                ok = true;
                                break;
                            }
                            _ => {
                                eprintln!("hvf-el1: unknown HVC {x0}");
                                dump(vcpu, &ex);
                                break;
                            }
                        }
                    }
                    0x24 => {
                        // data abort from EL1 to EL2: the stage-2 miss = our MMIO probe
                        let ipa = ex.exception.physical_address;
                        let isv = syn & (1 << 24) != 0;
                        let srt = ((syn >> 16) & 0x1f) as u32;
                        let iswrite = syn & (1 << 6) != 0;
                        if ipa >= MMIO_BASE && ipa < MMIO_BASE + MMIO_SIZE && isv {
                            if !iswrite && srt != 31 {
                                unsafe { hv_vcpu_set_reg(vcpu, HV_REG_X0 + srt, 0x1234_0000 + (exits & 0xffff)) };
                            }
                            let pc = reg(vcpu, HV_REG_PC);
                            unsafe { hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4) };
                        } else {
                            eprintln!("hvf-el1: unexpected stage-2 data abort");
                            dump(vcpu, &ex);
                            break;
                        }
                    }
                    _ => {
                        eprintln!("hvf-el1: unexpected exception exit");
                        dump(vcpu, &ex);
                        break;
                    }
                }
            }
            other => {
                eprintln!("hvf-el1: unknown exit reason {other}");
                dump(vcpu, &ex);
                break;
            }
        }
    }
    let t_end = ticks();
    println!(
        "host: guest {} after {} exits, {:.2} s of vCPU wall time",
        if ok { "finished" } else { "FAILED" },
        exits,
        (t_end - t_start) as f64 / counter_hz() as f64
    );
    unsafe {
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
    }
    ok
}

// --------------------------------------------------------- native baseline
fn native_set(name: &str, pages: usize, mem: *mut u8, rng: &mut Rng, freq: u64) {
    let perm = vec![0u32; pages].leak().as_mut_ptr();
    let idx_n = 1usize << 20;
    let idx = vec![0u32; idx_n].leak().as_mut_ptr();
    let entries = tlb_entries_for(pages);
    let table = vec![0u64; entries * 4 + 8].leak();
    let table_ptr = unsafe { table.as_mut_ptr().add(8) };
    let mut env = [0u64; 2];
    let first = unsafe { build_chase(mem, pages, perm, rng) } as u64;
    unsafe {
        build_idx(idx, idx_n, pages, rng);
        build_tlb(env.as_mut_ptr(), table_ptr, entries, pages, mem as u64);
    }
    let steps = 2_000_000u64;
    let passes = 4u64;
    let base = mem as u64;
    let envp = env.as_ptr() as u64;
    unsafe { bench_chase_direct(base, first, steps / 4) };
    let t0 = ticks();
    let r1 = unsafe { bench_chase_direct(base, first, steps) };
    let t1 = ticks();
    let r2 = unsafe { bench_chase_softmmu(envp, first, steps) };
    let t2 = ticks();
    let r3 = unsafe { bench_chase_pinned(envp, first, steps) };
    let t3 = ticks();
    assert!(r1 == r2 && r2 == r3, "chase results differ");
    let f = |dt: u64, n: u64| {
        let (a, b) = ns(ps_per(dt, freq, n));
        format!("{a}.{b:02} ns")
    };
    println!(
        "load: {name:>7} chase  direct {}  softmmu {}  pinned {}   [{steps} steps, TLB {entries} entries]",
        f(t1 - t0, steps),
        f(t2 - t1, steps),
        f(t3 - t2, steps)
    );
    let mut sums = [0u64; 3];
    let t0 = ticks();
    for _ in 0..passes {
        sums[0] = sums[0].wrapping_add(unsafe { bench_sum_direct(base, idx as u64, idx_n as u64) });
    }
    let t1 = ticks();
    for _ in 0..passes {
        sums[1] = sums[1].wrapping_add(unsafe { bench_sum_softmmu(envp, idx as u64, idx_n as u64) });
    }
    let t2 = ticks();
    for _ in 0..passes {
        sums[2] = sums[2].wrapping_add(unsafe { bench_sum_pinned(envp, idx as u64, idx_n as u64) });
    }
    let t3 = ticks();
    assert!(sums[0] == sums[1] && sums[1] == sums[2], "sum results differ");
    let n = passes * idx_n as u64;
    println!(
        "load: {name:>7} indep  direct {}  softmmu {}  pinned {}   [{n} loads]",
        f(t1 - t0, n),
        f(t2 - t1, n),
        f(t3 - t2, n)
    );
}

fn native_baseline() {
    let freq = counter_hz();
    let mem = mmap_zero(48 << 20);
    for off in (0..(48 << 20)).step_by(4096) {
        unsafe { write_volatile(mem.add(off), 0) };
    }
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    println!("host: the same kernels natively in the macOS process (16 KiB host pages, no stage 2):");
    native_set("64 KiB", 16, mem, &mut rng, freq);
    native_set("4 MiB", 1024, mem, &mut rng, freq);
    native_set("8 MiB", 2048, mem, &mut rng, freq);
    native_set("16 MiB", 4096, mem, &mut rng, freq);
    native_set("32 MiB", 8192, mem, &mut rng, freq);
    native_movs(mem, freq);
    let _ = unsafe { read_volatile(mem) };
    native_jit(freq);
}

extern "C" {
    fn pthread_jit_write_protect_np(enabled: i32);
    fn sys_icache_invalidate(start: *mut c_void, len: usize);
}

/// What a JIT patch costs the QEMU process today: the macOS W^X toggle
/// pair around the write (patch 14 only removed the redundant ones).
/// The rep movsd kernels natively (see the payload's exp_movs), plus libc
/// memcpy per row as the floor.
fn native_movs(mem: *mut u8, freq: u64) {
    let w = 160u64;
    let table = vec![0u64; 4096 * 4 + 8].leak();
    let table_ptr = unsafe { table.as_mut_ptr().add(8) };
    let env = vec![0u64; 0x40].leak();   // mask/table + the kernels' env area
    for (name, rows) in [("32 KiB", 51u64), ("512 KiB", 819), ("8 MiB", 13107)] {
        let bytes = rows * w * 4;
        let pages = ((2 * bytes + 4095) / 4096) as usize;
        unsafe { build_tlb(env.as_mut_ptr(), table_ptr, 4096, pages, mem as u64) };
        let passes = ((96u64 << 20) / bytes).max(1);
        let n = passes * rows * w;
        let (t, chk) = unsafe {
            run_movs(mem as u64, env.as_ptr() as u64, rows, w, passes, ticks,
                     [bench_movs_today as MovsFn, bench_movs_today_nomb, bench_movs_softmmu, bench_movs_direct, bench_movs_fast])
        };
        if chk.iter().any(|&x| x != chk[0]) {
            println!("movs: {name:>7} kernels disagree or a probe refused: {chk:x?}");
            continue;
        }
        let t0 = ticks();
        for _ in 0..passes {
            for r in 0..rows {
                unsafe { memcpy(mem.add((bytes + r * w * 4) as usize), mem.add((r * w * 4) as usize), (w * 4) as usize) };
            }
        }
        let tm = ticks() - t0;
        let f = |dt: u64| {
            let (a, b) = ns(ps_per(dt, freq, n));
            format!("{a}.{b:02}")
        };
        println!(
            "movs: {name:>7} x2, rows of {} B  today {} ns/dword  no-mb {}  pinned+softmmu {}  pinned+direct {}  fast {}  memcpy {}   [{rows} rows, {passes} passes]",
            w * 4, f(t[0]), f(t[1]), f(t[2]), f(t[3]), f(t[4]), f(tm)
        );
        let (t, _) = unsafe {
            run_movs(mem as u64, env.as_ptr() as u64, rows, w, passes, ticks, [bench_movs_direct_nocc as MovsFn, bench_stos_direct, bench_lods_direct])
        };
        println!("movs: {name:>7} diag  direct-no-cc {}  stores-only {}  loads-only {}", f(t[0]), f(t[1]), f(t[2]));
        let (t, _) = unsafe {
            run_movs(mem as u64, env.as_ptr() as u64, rows, w, passes, ticks, [bench_diag_stenv as MovsFn, bench_diag_stenv_far, bench_diag_ldst_env, bench_diag_ststack, bench_diag_stwin_ldline])
        };
        println!("movs: {name:>7} diag2 st-env {}  st-env-far {}  ld+st-env {}  st-stack {}  st+ld-window-line {}", f(t[0]), f(t[1]), f(t[2]), f(t[3]), f(t[4]));
        {
        let (t, _) = unsafe {
            run_movs(mem as u64, env.as_ptr() as u64, rows, w, passes, ticks, [bench_diag_stenv_ldbase as MovsFn, bench_diag_stenv_stbase, bench_diag_stenv_ldstbase])
        };
        println!("movs: {name:>7} diag3 native: st-env+ld {}  st-env+st {}  st-env+ld+st {}", f(t[0]), f(t[1]), f(t[2]));
        }
    }
}

fn native_jit(freq: u64) {
    let p = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            16384,
            libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
            libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_JIT,
            -1,
            0,
        )
    };
    if p == libc::MAP_FAILED {
        println!("jit: MAP_JIT mmap failed (the host binary lacks the JIT entitlement); skipped");
        return;
    }
    let code = p as *mut u32;
    let f: extern "C" fn(u64, u64) -> u64 = unsafe { std::mem::transmute(p) };
    let n = 100_000u64;
    let mut bad = 0u64;
    let t0 = ticks();
    for i in 0..n {
        let imm = (i & 0xffff) as u32;
        unsafe {
            pthread_jit_write_protect_np(0);
            write_volatile(code, 0x5280_0000 | (imm << 5));
            write_volatile(code.add(1), 0x0b01_0000);
            write_volatile(code.add(2), 0xd65f_03c0);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(p, 12);
        }
        if f(0, 1) != imm as u64 + 1 {
            bad += 1;
        }
    }
    let dt = ticks() - t0;
    let (a, b) = ns(ps_per(dt, freq, n));
    println!("jit: native macOS: W^X toggle pair + patch + sys_icache_invalidate + call: {a}.{b:02} ns  [{n} rounds, {bad} wrong]");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).map(String::as_str).unwrap_or("build/hvf-el1/payload.bin");
    let payload = match std::fs::read(path) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("hvf-el1: cannot read payload {path}: {e}");
            std::process::exit(2);
        }
    };
    println!("hvf-el1: Hypervisor.framework EL1 probe (counter {} MHz)", counter_hz() / 1_000_000);
    let ok = if args.iter().any(|a| a == "--native-only") { true } else { run_guest(&payload) };
    native_baseline();
    std::process::exit(if ok { 0 } else { 1 });
}
