//! Raw ⇄ cooked sector layouts (doc 17 §2.2, §2.5): sync, BCD header,
//! Mode 2 subheader, synthesis of a raw sector from cooked user data, and
//! the kind of a stored raw sector.

use crate::ecc;
use crate::msf::Msf;
use crate::SectorKind;

pub const SYNC: [u8; 12] = [0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0];

pub fn bcd(v: u8) -> u8 {
    ((v / 10) << 4) | (v % 10)
}

pub fn from_bcd(v: u8) -> u8 {
    (v >> 4) * 10 + (v & 0x0F)
}

/// The 4-byte header of the sector at `lba`: BCD MSF of `lba + 150`, mode.
pub fn header(lba: i32, mode: u8) -> [u8; 4] {
    let m = Msf::from_lba(lba);
    [bcd(m.m), bcd(m.s), bcd(m.f), mode]
}

/// Sync + header of the given mode into `out`.
pub fn put_sync_header(lba: i32, mode: u8, out: &mut [u8; 2352]) {
    out[..12].copy_from_slice(&SYNC);
    out[12..16].copy_from_slice(&header(lba, mode));
}

/// A complete Mode 1 sector from 2048 user bytes.
pub fn build_mode1(lba: i32, data: &[u8], out: &mut [u8; 2352]) {
    debug_assert_eq!(data.len(), 2048);
    put_sync_header(lba, 1, out);
    out[16..2064].copy_from_slice(data);
    ecc::generate_mode1(out);
}

/// A complete Mode 2 form 1 sector from a subheader and 2048 user bytes.
pub fn build_mode2_form1(lba: i32, subheader: &[u8; 8], data: &[u8], out: &mut [u8; 2352]) {
    debug_assert_eq!(data.len(), 2048);
    put_sync_header(lba, 2, out);
    out[16..24].copy_from_slice(subheader);
    out[24..2072].copy_from_slice(data);
    ecc::generate_mode2_form1(out);
}

/// A complete Mode 2 form 2 sector from a subheader and 2324 user bytes.
pub fn build_mode2_form2(lba: i32, subheader: &[u8; 8], data: &[u8], out: &mut [u8; 2352]) {
    debug_assert_eq!(data.len(), 2324);
    put_sync_header(lba, 2, out);
    out[16..24].copy_from_slice(subheader);
    out[24..2348].copy_from_slice(data);
    ecc::generate_mode2_form2(out);
}

/// Sync + Mode 2 header in front of a stored 2336-byte body (subheader,
/// data, EDC/ECC as dumped).
pub fn build_mode2_2336(lba: i32, body: &[u8], out: &mut [u8; 2352]) {
    debug_assert_eq!(body.len(), 2336);
    put_sync_header(lba, 2, out);
    out[16..].copy_from_slice(body);
}

/// The kind of a stored raw data sector, from its sync and header. `None`
/// when there is no sync pattern (audio, or a damaged data sector).
pub fn detect_kind(raw: &[u8; 2352]) -> Option<SectorKind> {
    if raw[..12] != SYNC {
        return None;
    }
    match raw[15] {
        1 => Some(SectorKind::Mode1),
        2 => {
            if raw[18] & 0x20 != 0 {
                Some(SectorKind::Mode2Form2)
            } else {
                Some(SectorKind::Mode2Form1)
            }
        }
        _ => Some(SectorKind::Mode2Formless),
    }
}

/// Offset and length of the user data of a sector kind inside the raw
/// sector, or `None` for audio / gap.
pub fn user_data_range(kind: SectorKind) -> Option<(usize, usize)> {
    match kind {
        SectorKind::Mode1 => Some((16, 2048)),
        SectorKind::Mode2Form1 => Some((24, 2048)),
        SectorKind::Mode2Form2 => Some((24, 2324)),
        SectorKind::Mode2Formless => Some((16, 2336)),
        SectorKind::Audio | SectorKind::Gap => None,
    }
}

/// L-EC verification of a raw sector of the given kind.
pub fn verify(raw: &[u8; 2352], kind: SectorKind) -> ecc::Lec {
    match kind {
        SectorKind::Mode1 => ecc::verify_mode1(raw),
        SectorKind::Mode2Form1 => ecc::verify_mode2_form1(raw),
        SectorKind::Mode2Form2 => ecc::verify_mode2_form2(raw),
        _ => ecc::Lec::Ok,
    }
}
