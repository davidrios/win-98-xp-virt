// hvf-el1: setup shared by the EL1 payload and the host baseline (included
// with include!, so it must stay core-only). `mem` is where the setup
// writes (the identity arena in the VM, the mmap on the host); the kernels
// read the same bytes through `base` (the x86 window in the VM).

pub struct Rng(pub u64);
impl Rng {
    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
}

/// A random cyclic pointer chase over `pages` 4 KiB pages: one u32 per
/// page, on a random cache line, holding the offset of the next hop.
/// `perm` is scratch for `pages` u32s. Returns the first offset.
pub unsafe fn build_chase(mem: *mut u8, pages: usize, perm: *mut u32, rng: &mut Rng) -> u32 {
    for i in 0..pages {
        *perm.add(i) = i as u32;
    }
    for i in (1..pages).rev() {
        let j = (rng.next() % (i as u64 + 1)) as usize;
        let t = *perm.add(i);
        *perm.add(i) = *perm.add(j);
        *perm.add(j) = t;
    }
    let off = |p: u32, r: u64| -> u32 { (p << 12) | (((r % 1024) as u32) << 2) };
    let mut offs_first = 0u32;
    let mut prev_off = 0u32;
    for i in 0..pages {
        let o = off(*perm.add(i), rng.next());
        if i == 0 {
            offs_first = o;
        } else {
            *(mem.add(prev_off as usize) as *mut u32) = o;
        }
        prev_off = o;
    }
    *(mem.add(prev_off as usize) as *mut u32) = offs_first;
    offs_first
}

/// `n` independent random offsets into `pages` pages (word aligned).
pub unsafe fn build_idx(idx: *mut u32, n: usize, pages: usize, rng: &mut Rng) {
    for i in 0..n {
        let r = rng.next();
        let p = (r >> 20) % pages as u64;
        *idx.add(i) = ((p as u32) << 12) | (((r % 1024) as u32) << 2);
    }
}

/// QEMU's dynamic TLB grows to keep the use rate under 70 %; never below
/// 256 entries (CPU_TLB_DYN_MIN_BITS 6 ... 8 on the 4 KiB page targets).
pub fn tlb_entries_for(pages: usize) -> usize {
    let mut n = 256usize;
    while n * 7 < pages * 10 {
        n *= 2;
    }
    n
}

/// A softmmu TLB that always hits for linear pages [0, pages): entry
/// layout as CPUTLBEntry (addr_read @0, addr_write @8, addend @24, 32 bytes),
/// env layout as CPUTLBDescFast (mask @0, table @8; @16 is the movs kernels'
/// cc_op slot).
pub unsafe fn build_tlb(env: *mut u64, table: *mut u64, entries: usize, pages: usize, addend: u64) {
    for i in 0..entries * 4 {
        *table.add(i) = 0;
    }
    for p in 0..pages {
        let i = p & (entries - 1);
        *table.add(i * 4) = (p as u64) << 12;       // addr_read
        *table.add(i * 4 + 1) = (p as u64) << 12;   // addr_write
        *table.add(i * 4 + 3) = addend;
    }
    *env = ((entries as u64) - 1) << 5;
    *env.add(1) = table as u64;
}

pub fn ps_per(ticks: u64, freq: u64, n: u64) -> u64 {
    ((ticks as u128) * 1_000_000_000_000u128 / ((freq as u128) * (n as u128))) as u64
}

/// picoseconds -> (ns, hundredths)
pub fn ns(ps: u64) -> (u64, u64) {
    (ps / 1000, (ps % 1000) / 10)
}

/// The `rep movsd` kernels of bench.S: `rows` rows of `w` dwords copied from
/// guest offset 0 to offset rows*w*4 (both inside the TLB's linear pages),
/// `passes` times each kernel; per kernel the ticks and a hash of the
/// destination (u64::MAX = its probe refused).  `base` is the window (VM)
/// or the arena (native); `env` the fake CPUTLBDescFast (mask @0, table @8;
/// [env + 0x10, env + 0x130) belongs to the kernels).  Copying kernels must
/// agree on the hash; the load-only / store-only diagnostics do not.
pub type MovsFn = unsafe extern "C" fn(u64, u64, u64, u64, u64, u64) -> u64;
pub unsafe fn run_movs<const N: usize>(
    base: u64, env: u64, rows: u64, w: u64, passes: u64, ticks: fn() -> u64, fns: [MovsFn; N],
) -> ([u64; N], [u64; N]) {
    let dst = rows * w * 4;
    let mut t = [0u64; N];
    let mut check = [0u64; N];
    for (k, f) in fns.iter().enumerate() {
        for i in 0..(dst / 8) {
            *((base + dst) as *mut u64).add(i as usize) = 0;
        }
        if f(base, 0, dst, rows, w, env) != 0 {
            check[k] = u64::MAX;
            continue;
        }
        let t0 = ticks();
        for _ in 0..passes {
            f(base, 0, dst, rows, w, env);
        }
        t[k] = ticks() - t0;
        let mut x = 0u64;
        for i in 0..(dst / 8) {
            x = x.wrapping_mul(0x9E37_79B9_7F4A_7C15).wrapping_add(*((base + dst) as *const u64).add(i as usize));
        }
        check[k] = x;
    }
    (t, check)
}
