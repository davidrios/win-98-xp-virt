//! The mirrored page tables: an x86-style two-level table living in "x86
//! RAM" (the guest OS would write it) is walked on demand by the EL1 data
//! abort handler, which installs the matching stage-1 PTE in a 4 GiB VA
//! window. One stage-1 root per x86 CR3, tagged with an ASID, so a CR3
//! switch is a TTBR0 write and nothing is refaulted.

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

/// The arena's address: the host picks it (1 GiB aligned, the same in the
/// host process, as IPA and as guest VA), start.S stores it here.
#[no_mangle]
pub static mut ARENA_BASE: u64 = 0;

/// Invalidate with the non-broadcast `tlbi vae1` (one vCPU, the host
/// handles migration between cores) instead of `vae1is`.
pub static mut TLBI_LOCAL: bool = false;

#[inline(always)]
pub fn arena() -> u64 {
    unsafe { ARENA_BASE }
}

pub const WIN0: u64 = 0x10_0000_0000;
pub const X86RAM_SIZE: u64 = 48 << 20;

/// x86 physical 0
#[inline(always)]
pub fn x86ram() -> u64 {
    arena() + (16 << 20)
}

const ATTR_NORMAL: u64 = (1 << 10) | (3 << 8); // AF, inner shareable, AttrIdx 0
const AP_RO: u64 = 2 << 6;
const NG: u64 = 1 << 11;
const UXN: u64 = 1 << 54;
const TABLE: u64 = 3;
const PAGE: u64 = 3;

// x86 PTE bits
pub const X_P: u32 = 1;
pub const X_RW: u32 = 2;
pub const X_US: u32 = 4;
pub const X_A: u32 = 0x20;
pub const X_D: u32 = 0x40;

extern "C" {
    static mut pt_l1: [u64; 512];
}

/// Bump allocator of zeroed 4 KiB stage-1 tables (in the arena).
pub struct Pool {
    pub next: u64,
    pub end: u64,
}

impl Pool {
    pub fn alloc(&mut self) -> *mut u64 {
        let p = self.next;
        if p + 4096 > self.end {
            panic!("stage-1 table pool exhausted");
        }
        self.next += 4096;
        unsafe { core::ptr::write_bytes(p as *mut u8, 0, 4096) };
        p as *mut u64
    }
}

/// Bump allocator of x86 physical pages for the synthetic x86 tables.
pub struct X86Alloc {
    pub next_page: u32,
}

impl X86Alloc {
    pub fn page(&mut self) -> u32 {
        let p = self.next_page;
        self.next_page += 1;
        let va = x86ram() + ((p as u64) << 12);
        if va + 4096 > x86ram() + X86RAM_SIZE {
            panic!("x86 RAM exhausted");
        }
        unsafe { core::ptr::write_bytes(va as *mut u8, 0, 4096) };
        p
    }
}

#[inline(always)]
pub fn dsb_ishst() {
    unsafe { asm!("dsb ishst", options(nostack)) }
}

/// An address space: one stage-1 root (L0 -> private L1 with the shared
/// identity entries copied in) + ASID + the x86 CR3 it mirrors.
pub struct Space {
    pub l0: u64,
    pub l1: u64,
    pub asid: u64,
    pub cr3: u32,
}

impl Space {
    pub fn new(pool: &mut Pool, asid: u64, cr3: u32) -> Space {
        let l0 = pool.alloc();
        let l1 = pool.alloc();
        unsafe {
            // the identity entries (image, arena, device) are global; the
            // window entries (64..68) are empty in pt_l1 and per space
            let src = core::ptr::addr_of!(pt_l1) as *const u64;
            for i in 0..512 {
                write_volatile(l1.add(i), read_volatile(src.add(i)));
            }
            write_volatile(l0, (l1 as u64) | TABLE);
        }
        Space { l0: l0 as u64, l1: l1 as u64, asid, cr3 }
    }

    pub fn ttbr(&self) -> u64 {
        self.l0 | (self.asid << 48)
    }

    /// The CR3 switch: a TTBR0 write with the ASID, no TLB invalidation.
    #[inline(always)]
    pub fn activate(&self) {
        let v = self.ttbr();
        unsafe { asm!("dsb ishst", "msr ttbr0_el1, {v}", "isb", v = in(reg) v, options(nostack)) }
    }

    fn next_level(pool: &mut Pool, table: *mut u64, idx: usize) -> *mut u64 {
        unsafe {
            let e = read_volatile(table.add(idx));
            if e & 1 != 0 {
                (e & 0x0000_ffff_ffff_f000) as *mut u64
            } else {
                let t = pool.alloc();
                write_volatile(table.add(idx), (t as u64) | TABLE);
                t
            }
        }
    }

    /// Install (or upgrade) the stage-1 PTE for `va` -> `pa`.
    pub fn install(&self, pool: &mut Pool, va: u64, pa: u64, ro: bool) {
        let l1 = self.l1 as *mut u64;
        let l2 = Self::next_level(pool, l1, ((va >> 30) & 511) as usize);
        let l3 = Self::next_level(pool, l2, ((va >> 21) & 511) as usize);
        let i3 = ((va >> 12) & 511) as usize;
        let mut pte = pa | ATTR_NORMAL | NG | UXN | PAGE;
        if ro {
            pte |= AP_RO;
        }
        unsafe {
            let old = read_volatile(l3.add(i3));
            write_volatile(l3.add(i3), pte);
            dsb_ishst();
            if old & 1 != 0 {
                // a permission change: the stale entry may be cached
                let op = ((va >> 12) & 0xfff_ffff_ffff) | (self.asid << 48);
                if TLBI_LOCAL {
                    asm!("tlbi vae1, {op}", "dsb nsh", "isb", op = in(reg) op, options(nostack));
                } else {
                    asm!("tlbi vae1is, {op}", "dsb ish", "isb", op = in(reg) op, options(nostack));
                }
            }
        }
    }

    /// The flush model (what QEMU does on a CR3 write): drop every
    /// mirrored page of the window and the TLB entries of the ASID.
    pub fn clear_window(&self) {
        let l1 = self.l1 as *mut u64;
        unsafe {
            for i1 in 64..68 {
                let e1 = read_volatile(l1.add(i1));
                if e1 & 1 == 0 {
                    continue;
                }
                let l2 = (e1 & 0x0000_ffff_ffff_f000) as *mut u64;
                for i2 in 0..512 {
                    let e2 = read_volatile(l2.add(i2));
                    if e2 & 1 == 0 {
                        continue;
                    }
                    let l3 = (e2 & 0x0000_ffff_ffff_f000) as *mut u64;
                    for i3 in 0..512 {
                        write_volatile(l3.add(i3), 0);
                    }
                }
            }
            let op = self.asid << 48;
            asm!("dsb ishst", "tlbi aside1is, {op}", "dsb ish", "isb", op = in(reg) op, options(nostack));
        }
    }
}

// ------------------------------------------------------------ x86 tables

fn x86_page(phys: u32) -> *mut u32 {
    (x86ram() + ((phys as u64) & !0xfff)) as *mut u32
}

/// Map `linear` -> `phys` (4 KiB pages) in the x86 tables rooted at `cr3`.
pub fn x86_map(alloc: &mut X86Alloc, cr3: u32, linear: u32, phys: u32, flags: u32) {
    unsafe {
        let pd = x86_page(cr3);
        let i = (linear >> 22) as usize;
        let mut pde = read_volatile(pd.add(i));
        if pde & X_P == 0 {
            let pt = alloc.page() << 12;
            pde = pt | X_P | X_RW | X_US;
            write_volatile(pd.add(i), pde);
        }
        let pt = x86_page(pde);
        write_volatile(pt.add(((linear >> 12) & 1023) as usize), (phys << 12) | flags | X_P);
    }
}

pub fn x86_pte(cr3: u32, linear: u32) -> u32 {
    unsafe {
        let pde = read_volatile(x86_page(cr3).add((linear >> 22) as usize));
        if pde & X_P == 0 {
            return 0;
        }
        read_volatile(x86_page(pde).add(((linear >> 12) & 1023) as usize))
    }
}

/// mmu_translate() in miniature: the walk, the A/D updates, the #PF error
/// code; the result says how to mirror the page (RW only once D is set,
/// so the first write faults and sets it, like tlb_set_page's rule).
pub fn x86_walk(cr3: u32, linear: u32, write: bool) -> Result<(u64, bool), u32> {
    let err_w = if write { 2 } else { 0 };
    unsafe {
        let pd = x86_page(cr3);
        let pde = read_volatile(pd.add((linear >> 22) as usize));
        if pde & X_P == 0 {
            return Err(err_w);
        }
        let pt = x86_page(pde);
        let pi = ((linear >> 12) & 1023) as usize;
        let mut pte = read_volatile(pt.add(pi));
        if pte & X_P == 0 {
            return Err(err_w);
        }
        if write && pte & X_RW == 0 {
            return Err(err_w | 1);
        }
        pte |= X_A;
        if write {
            pte |= X_D;
        }
        write_volatile(pt.add(pi), pte);
        let ro = pte & X_RW == 0 || pte & X_D == 0;
        Ok((x86ram() + ((pte & !0xfff) as u64), ro))
    }
}

/// The data abort handler's work for a window address.
pub fn handle_fault(space: &Space, pool: &mut Pool, far: u64, write: bool) -> Result<(), u32> {
    let linear = (far - WIN0) as u32;
    let (pa, ro) = x86_walk(space.cr3, linear, write)?;
    space.install(pool, far & !0xfff, pa, ro);
    Ok(())
}

pub fn in_window(far: u64) -> bool {
    far >= WIN0 && far < WIN0 + (1 << 32)
}
