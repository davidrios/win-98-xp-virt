//! MSF (minute:second:frame) ↔ LBA addressing.
//!
//! CD addressing: 75 frames per second, 60 seconds per minute, and a 150
//! frame (2 second) offset between the start of the program area (MSF
//! 00:02:00) and LBA 0. MMC commands use both; protection code cares that we
//! get the edges right. Checked by `discx selftest`'s `msf` check (no unit
//! tests: CLAUDE.md policy).

/// Frames (sectors) per second on CD.
pub const FRAMES_PER_SECOND: u32 = 75;
/// LBA 0 sits at MSF 00:02:00.
pub const MSF_OFFSET: i32 = 150;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Msf {
    pub m: u8,
    pub s: u8,
    pub f: u8,
}

impl Msf {
    /// Absolute MSF → LBA. `00:02:00` → 0. Values below 2 seconds map to
    /// negative LBAs (lead-in addressing), which is intentional.
    pub fn to_lba(self) -> i32 {
        (self.m as i32 * 60 + self.s as i32) * FRAMES_PER_SECOND as i32 + self.f as i32 - MSF_OFFSET
    }

    /// LBA → absolute MSF. Panics if the result would not fit in 99 minutes
    /// (beyond any real disc).
    pub fn from_lba(lba: i32) -> Msf {
        let abs = lba + MSF_OFFSET;
        assert!(abs >= 0, "LBA {lba} precedes lead-in addressable range");
        let abs = abs as u32;
        let m = abs / (60 * FRAMES_PER_SECOND);
        assert!(m <= 99, "LBA {lba} beyond 99-minute MSF range");
        let s = (abs / FRAMES_PER_SECOND) % 60;
        let f = abs % FRAMES_PER_SECOND;
        Msf {
            m: m as u8,
            s: s as u8,
            f: f as u8,
        }
    }
}
