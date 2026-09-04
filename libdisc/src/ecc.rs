//! EDC (CRC-32) and RSPC P/Q parity for CD-ROM data sectors (doc 17 §2.5).
//!
//! The two-pass loop is the public-domain form from Neill Corlett's ECM
//! tool (libmirage uses the same). Verification only: a mismatch is what a
//! real drive reports as an L-EC failure, and that is the SafeDisc signal;
//! nothing is corrected.

use std::sync::OnceLock;

struct Tables {
    f: [u8; 256],
    b: [u8; 256],
    edc: [u32; 256],
}

fn tables() -> &'static Tables {
    static T: OnceLock<Tables> = OnceLock::new();
    T.get_or_init(|| {
        let mut t = Tables { f: [0; 256], b: [0; 256], edc: [0; 256] };
        for i in 0..256usize {
            let j = ((i << 1) ^ if i & 0x80 != 0 { 0x11D } else { 0 }) & 0xFF;
            t.f[i] = j as u8;
            t.b[i ^ j] = i as u8;
            let mut edc = i as u32;
            for _ in 0..8 {
                edc = (edc >> 1) ^ if edc & 1 != 0 { 0xD801_8001 } else { 0 };
            }
            t.edc[i] = edc;
        }
        t
    })
}

/// EDC over `src`, continuing from `edc` (start at 0).
pub fn edc_compute(mut edc: u32, src: &[u8]) -> u32 {
    let t = tables();
    for &b in src {
        edc = (edc >> 8) ^ t.edc[((edc ^ b as u32) & 0xFF) as usize];
    }
    edc
}

/// One RSPC pass. `address` is the 4-byte header (zeros for Mode 2), `data`
/// starts at sector byte 16 and must cover the pass's source size minus 4.
fn write_pq(
    address: &[u8; 4],
    data: &[u8],
    major_count: usize,
    minor_count: usize,
    major_mult: usize,
    minor_inc: usize,
    ecc: &mut [u8],
) {
    let t = tables();
    let size = major_count * minor_count;
    for major in 0..major_count {
        let mut index = (major >> 1) * major_mult + (major & 1);
        let mut ecc_a: u8 = 0;
        let mut ecc_b: u8 = 0;
        for _ in 0..minor_count {
            let temp = if index < 4 { address[index] } else { data[index - 4] };
            index += minor_inc;
            if index >= size {
                index -= size;
            }
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = t.f[ecc_a as usize];
        }
        ecc_a = t.b[(t.f[ecc_a as usize] ^ ecc_b) as usize];
        ecc[major] = ecc_a;
        ecc[major + major_count] = ecc_a ^ ecc_b;
    }
}

/// Compute the 276 parity bytes (P then Q) for a sector whose bytes 16..2076
/// (data + EDC + zero/reserved) are in `data` and whose header is `address`.
/// `data` must be the 2236 bytes 16..2252 of the sector: the Q pass reads
/// the P parity, so P is written into `data` first.
fn pq(address: &[u8; 4], sector: &mut [u8; 2352]) {
    let (body, parity) = sector.split_at_mut(2076);
    let (p, q) = parity.split_at_mut(172);
    write_pq(address, &body[16..], 86, 24, 2, 86, p);
    // the Q pass reads bytes 16..2248 (data + edc + zero + P)
    let mut src = [0u8; 2232];
    src[..2060].copy_from_slice(&body[16..2076]);
    src[2060..].copy_from_slice(p);
    write_pq(address, &src, 52, 43, 86, 88, q);
}

/// Fill EDC and P/Q parity of a Mode 1 sector whose sync, header and
/// 2048 data bytes are already in place.
pub fn generate_mode1(sector: &mut [u8; 2352]) {
    let edc = edc_compute(0, &sector[..2064]);
    sector[2064..2068].copy_from_slice(&edc.to_le_bytes());
    sector[2068..2076].fill(0);
    let mut address = [0u8; 4];
    address.copy_from_slice(&sector[12..16]);
    pq(&address, sector);
}

/// Fill EDC and P/Q parity of a Mode 2 form 1 sector (sync, header,
/// subheader, data in place). The header is zero for the parity computation.
pub fn generate_mode2_form1(sector: &mut [u8; 2352]) {
    let edc = edc_compute(0, &sector[16..2072]);
    sector[2072..2076].copy_from_slice(&edc.to_le_bytes());
    pq(&[0u8; 4], sector);
}

/// Fill the EDC of a Mode 2 form 2 sector (2324 data bytes at 24..2348).
pub fn generate_mode2_form2(sector: &mut [u8; 2352]) {
    let edc = edc_compute(0, &sector[16..2348]);
    sector[2348..2352].copy_from_slice(&edc.to_le_bytes());
}

/// Result of an L-EC check.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Lec {
    Ok,
    EdcMismatch,
    EccMismatch,
    /// No sync pattern / wrong mode byte: not a data sector at all (an
    /// audio-format tail of a data track, a zero-filled unreadable sector).
    NoSync,
}

impl Lec {
    pub fn is_ok(self) -> bool {
        self == Lec::Ok
    }
}

/// Recompute EDC and parity of a Mode 1 sector and compare with the stored bytes.
pub fn verify_mode1(raw: &[u8; 2352]) -> Lec {
    let edc = edc_compute(0, &raw[..2064]);
    if edc.to_le_bytes() != raw[2064..2068] {
        return Lec::EdcMismatch;
    }
    let mut scratch = *raw;
    let mut address = [0u8; 4];
    address.copy_from_slice(&raw[12..16]);
    pq(&address, &mut scratch);
    if scratch[2076..] != raw[2076..] {
        return Lec::EccMismatch;
    }
    Lec::Ok
}

/// Same for a Mode 2 form 1 sector (header excluded from the parity).
pub fn verify_mode2_form1(raw: &[u8; 2352]) -> Lec {
    let edc = edc_compute(0, &raw[16..2072]);
    if edc.to_le_bytes() != raw[2072..2076] {
        return Lec::EdcMismatch;
    }
    let mut scratch = *raw;
    pq(&[0u8; 4], &mut scratch);
    if scratch[2076..] != raw[2076..] {
        return Lec::EccMismatch;
    }
    Lec::Ok
}

/// Mode 2 form 2: EDC only, and a zero EDC means "not recorded" (allowed).
pub fn verify_mode2_form2(raw: &[u8; 2352]) -> Lec {
    if raw[2348..2352] == [0, 0, 0, 0] {
        return Lec::Ok;
    }
    let edc = edc_compute(0, &raw[16..2348]);
    if edc.to_le_bytes() != raw[2348..2352] {
        return Lec::EdcMismatch;
    }
    Lec::Ok
}

/// C2 error pointers, approximate (doc 17 §2.5): one bit per sector byte,
/// set for every byte where the recomputed EDC/parity disagrees with the
/// stored one. Audio and gap sectors: all clear. Returns the number of
/// bits set.
pub fn c2_bits(raw: &[u8; 2352], kind: crate::SectorKind, out: &mut [u8; 294]) -> u32 {
    out.fill(0);
    if kind.is_data() && !crate::sector::has_sync_header(raw, kind) {
        out.fill(0xFF);
        return 2352;
    }
    let mut scratch = *raw;
    let (edc_range, edc_at, parity) = match kind {
        crate::SectorKind::Mode1 => {
            let mut address = [0u8; 4];
            address.copy_from_slice(&raw[12..16]);
            pq(&address, &mut scratch);
            (0..2064, 2064usize, true)
        }
        crate::SectorKind::Mode2Form1 => {
            pq(&[0u8; 4], &mut scratch);
            (16..2072, 2072, true)
        }
        crate::SectorKind::Mode2Form2 => (16..2348, 2348, false),
        _ => return 0,
    };
    let edc = edc_compute(0, &raw[edc_range]).to_le_bytes();
    scratch[edc_at..edc_at + 4].copy_from_slice(&edc);
    let mut n = 0;
    let end = if parity { 2352 } else { 2352 };
    for i in 0..end {
        if scratch[i] != raw[i] {
            out[i / 8] |= 0x80 >> (i % 8);
            n += 1;
        }
    }
    n
}
