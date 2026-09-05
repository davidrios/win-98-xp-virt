//! Subchannel synthesis (doc 17 §2.6): P (pause flag), Q ADR 1 position
//! frames with ADR 2 (MCN) / ADR 3 (ISRC) interleaved, CRC-16, and the
//! deinterleaved ⇄ interleaved conversions the READ CD responder needs.

use crate::msf::Msf;
use crate::sector::bcd;
use crate::Disc;

/// CRC-16 of a Q frame (CCITT polynomial 0x1021, init 0, inverted), as
/// stored big-endian in bytes 10..12.
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    !crc
}

/// True when the 12-byte Q frame's CRC matches.
pub fn q_crc_ok(q: &[u8]) -> bool {
    q.len() >= 12 && crc16(&q[..10]).to_be_bytes() == q[10..12]
}

fn put_msf(out: &mut [u8], lba_abs: i32) {
    let m = Msf::from_lba(lba_abs - crate::msf::MSF_OFFSET);
    out[0] = bcd(m.m);
    out[1] = bcd(m.s);
    out[2] = bcd(m.f);
}

/// ISRC character to its 6-bit Red Book code.
fn isrc_code(c: u8) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'A'..=b'Z' => c - b'A' + 0x11,
        b'a'..=b'z' => c - b'a' + 0x11,
        _ => 0,
    }
}

/// The 96 deinterleaved subchannel bytes of `lba` when no dump provides them.
pub fn synthesize(disc: &Disc, lba: i32, out: &mut [u8; 96]) {
    out.fill(0);
    if disc.pause_at(lba) {
        out[..12].fill(0xFF);
    }
    let q = &mut out[12..24];
    let aframe = bcd(Msf::from_lba(lba).f);
    match disc.locate(lba) {
        None => {
            // between sessions: lead-out style frame of the previous session
            q[0] = 0x01;
            q[1] = 0xAA;
            q[2] = 0x01;
            put_msf(&mut q[3..6], 0);
            put_msf(&mut q[7..10], lba + crate::msf::MSF_OFFSET);
        }
        Some((_, t, index)) => {
            let phase = lba.rem_euclid(100);
            if phase == 98 && disc.mcn.is_some() {
                let mcn = disc.mcn.unwrap();
                q[0] = (t.control << 4) | 2;
                for (i, d) in mcn.iter().enumerate() {
                    let v = d.wrapping_sub(b'0') & 0x0F;
                    if i % 2 == 0 {
                        q[1 + i / 2] |= v << 4;
                    } else {
                        q[1 + i / 2] |= v;
                    }
                }
                q[8] = 0;
                q[9] = aframe;
            } else if phase == 99 && t.isrc.is_some() {
                let isrc = t.isrc.unwrap();
                q[0] = (t.control << 4) | 3;
                let c: Vec<u8> = isrc.iter().map(|&x| isrc_code(x)).collect();
                q[1] = (c[0] << 2) | (c[1] >> 4);
                q[2] = (c[1] << 4) | (c[2] >> 2);
                q[3] = (c[2] << 6) | c[3];
                q[4] = c[4] << 2;
                let digits: Vec<u8> = isrc[5..12].iter().map(|&x| x.wrapping_sub(b'0') & 0x0F).collect();
                q[5] = (digits[0] << 4) | digits[1];
                q[6] = (digits[2] << 4) | digits[3];
                q[7] = (digits[4] << 4) | digits[5];
                q[8] = digits[6] << 4;
                q[9] = aframe;
            } else {
                q[0] = (t.control << 4) | 1;
                q[1] = bcd(t.number);
                q[2] = bcd(index);
                let rel = if index == 0 { t.start_lba - lba } else { lba - t.start_lba };
                put_msf(&mut q[3..6], rel);
                q[6] = 0;
                put_msf(&mut q[7..10], lba + crate::msf::MSF_OFFSET);
            }
        }
    }
    let crc = crc16(&q[..10]).to_be_bytes();
    q[10] = crc[0];
    q[11] = crc[1];
}

/// Deinterleaved P..W (8 × 12 bytes) → the 96-byte interleaved form real
/// drives deliver (bit 7 of each byte = P, bit 6 = Q, … bit 0 = W).
pub fn interleave(de: &[u8; 96], out: &mut [u8; 96]) {
    out.fill(0);
    for ch in 0..8 {
        for i in 0..96 {
            let bit = (de[ch * 12 + i / 8] >> (7 - (i % 8))) & 1;
            out[i] |= bit << (7 - ch);
        }
    }
}

/// The inverse of `interleave`.
pub fn deinterleave(inter: &[u8; 96], out: &mut [u8; 96]) {
    out.fill(0);
    for ch in 0..8 {
        for i in 0..96 {
            let bit = (inter[i] >> (7 - ch)) & 1;
            out[ch * 12 + i / 8] |= bit << (7 - (i % 8));
        }
    }
}
