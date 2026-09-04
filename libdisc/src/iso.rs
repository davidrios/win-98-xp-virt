//! A plain cooked image: one Mode 1 data track over 2048-byte sectors.

use std::path::Path;

use crate::{Disc, Error, Extent, Layout, Result, Session, Source, Track, TrackMode};

pub fn open(path: &Path) -> Result<Disc> {
    let mut disc = Disc::new();
    let file = disc.add_file(path)?;
    let len = disc.file(file).len;
    if len == 0 || len % 2048 != 0 {
        return Err(Error::Invalid(format!("{}: size {} is not a multiple of 2048", path.display(), len)));
    }
    let count = (len / 2048) as u32;
    disc.sessions.push(Session {
        number: 1,
        tracks: vec![Track {
            number: 1,
            mode: TrackMode::Mode1,
            control: 0x4,
            isrc: None,
            indices: vec![(1, 0)],
            start_lba: 0,
            end_lba: count as i32,
            extents: vec![Extent { lba: 0, count, source: Source::File { file, offset: 0, layout: Layout::Cooked2048, swap: false }, sub: None }],
        }],
        leadout_lba: count as i32,
    });
    Ok(disc)
}
