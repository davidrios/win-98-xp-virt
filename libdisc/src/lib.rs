//! libdisc — raw optical disc model (design doc 05).
//!
//! M0 state: addressing primitives and the disc-model vocabulary types.
//! Format parsers (cue/bin first), subchannel synthesis, C2 annotations, and
//! the C API for QEMU's ATAPI shim land in M5. Behavioral reference:
//! libmirage (not linked).

pub mod msf;

/// Raw sector payload size on CD (bytes).
pub const RAW_SECTOR_SIZE: usize = 2352;
/// Cooked Mode 1 user-data size (bytes).
pub const COOKED_SECTOR_SIZE: usize = 2048;
/// Subchannel data per sector, deinterleaved P..W (bytes).
pub const SUBCHANNEL_SIZE: usize = 96;

/// Track mode as recorded in the TOC / cue sheet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackMode {
    /// Red Book audio.
    Audio,
    /// Yellow Book Mode 1 data (2048 user bytes).
    Mode1,
    /// Mode 2 (form 1/form 2 decided per sector).
    Mode2,
}

/// One track of a session.
#[derive(Debug, Clone)]
pub struct Track {
    /// Track number, 1-based (1..=99).
    pub number: u8,
    pub mode: TrackMode,
    /// Absolute LBA of index 01 (track start proper).
    pub start_lba: i32,
    /// Length in sectors, indices 00 gap excluded.
    pub length: u32,
}

/// The whole disc: sessions later; single-session model for now.
#[derive(Debug, Clone, Default)]
pub struct Disc {
    pub tracks: Vec<Track>,
}

impl Disc {
    /// Total sectors covered by tracks (lead-in/out excluded).
    pub fn sector_count(&self) -> u32 {
        self.tracks.iter().map(|t| t.length).sum()
    }
}
