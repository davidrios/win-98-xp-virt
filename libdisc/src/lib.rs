//! libdisc — raw optical disc model for the QEMU CD-ROM backend (docs 05
//! and 17).
//!
//! Sessions, tracks, indices and per-sector kinds over cue/bin, CloneCD and
//! plain ISO images; raw ⇄ cooked synthesis with EDC/ECC (verified, never
//! corrected); Q-channel synthesis; the MMC responders (`mmc`) and the C
//! API (`capi`) that QEMU's `cdimage` block driver and `atapi.c` use. Reads
//! are synchronous `pread`s on immutable files; a `Disc` is immutable once
//! opened and every method is safe to call from several threads.

#[allow(non_camel_case_types)]
pub mod capi;
pub mod ccd;
pub mod cue;
pub mod ecc;
pub mod iso;
pub mod mds;
pub mod mmc;
pub mod msf;
pub mod sector;
pub mod subq;

use std::fmt;
use std::fs::File;
use std::path::{Path, PathBuf};

/// Raw sector payload size on CD (bytes).
pub const RAW_SECTOR_SIZE: usize = 2352;
/// Cooked Mode 1 / Mode 2 form 1 user-data size (bytes).
pub const COOKED_SECTOR_SIZE: usize = 2048;
/// Subchannel data per sector, deinterleaved P..W (bytes).
pub const SUBCHANNEL_SIZE: usize = 96;
/// The mandatory pause before a track's index 01, in sectors.
pub const PREGAP_SECTORS: i32 = 150;

/// Every failure a caller can see; the C API maps the variants to
/// `LIBDISC_E*` codes and atapi.c to sense keys (doc 17 §3).
#[derive(Debug)]
pub enum Error {
    /// LBA outside `[0, lead-out)`.
    Range,
    /// L-EC failed on a cooked read of a data sector.
    Medium,
    /// Wrong sector kind for the request (audio / gap / form 2 as cooked).
    Mode,
    /// Bad parameter, bad CDB field, or an image that does not parse.
    Invalid(String),
    /// A host file could not be read.
    Io(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Range => write!(f, "LBA out of range"),
            Error::Medium => write!(f, "L-EC uncorrectable error"),
            Error::Mode => write!(f, "illegal mode for this track"),
            Error::Invalid(s) => write!(f, "{s}"),
            Error::Io(s) => write!(f, "I/O error: {s}"),
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e.to_string())
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Track mode as recorded in the TOC / cue sheet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackMode {
    Audio,
    Mode1,
    /// Mode 2, form decided per sector from the subheader (XA).
    Mode2,
    Mode2Form1,
    Mode2Form2,
    /// Mode 2 without subheader interpretation (CD-I).
    Mode2Formless,
}

impl TrackMode {
    pub fn is_data(self) -> bool {
        self != TrackMode::Audio
    }
    /// Header mode byte for synthesized sectors.
    pub fn header_mode(self) -> u8 {
        match self {
            TrackMode::Audio => 0,
            TrackMode::Mode1 => 1,
            _ => 2,
        }
    }
    /// The kind of a synthesized (zero-filled or cooked-sourced) sector of this mode.
    pub fn default_kind(self) -> SectorKind {
        match self {
            TrackMode::Audio => SectorKind::Audio,
            TrackMode::Mode1 => SectorKind::Mode1,
            TrackMode::Mode2 | TrackMode::Mode2Form1 => SectorKind::Mode2Form1,
            TrackMode::Mode2Form2 => SectorKind::Mode2Form2,
            TrackMode::Mode2Formless => SectorKind::Mode2Formless,
        }
    }
}

/// What one sector is, as `libdisc_sector_info.kind` reports it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SectorKind {
    Audio,
    Mode1,
    Mode2Form1,
    Mode2Form2,
    Mode2Formless,
    /// Not covered by any track (between sessions).
    Gap,
}

impl SectorKind {
    /// `LIBDISC_KIND_*` value.
    pub fn code(self) -> u8 {
        match self {
            SectorKind::Audio => 0,
            SectorKind::Mode1 => 1,
            SectorKind::Mode2Form1 => 2,
            SectorKind::Mode2Form2 => 3,
            SectorKind::Mode2Formless => 4,
            SectorKind::Gap => 5,
        }
    }
    pub fn is_data(self) -> bool {
        !matches!(self, SectorKind::Audio | SectorKind::Gap)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SectorInfo {
    pub kind: SectorKind,
    pub track: u8,
    pub index: u8,
    /// Data sectors only: did EDC and parity verify.
    pub lec_ok: Option<bool>,
}

/// One raw TOC entry as a CloneCD `[Entry]` records it, replayed verbatim
/// by READ TOC format 2.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TocEntry {
    pub session: u8,
    pub point: u8,
    pub adr: u8,
    pub control: u8,
    pub min: u8,
    pub sec: u8,
    pub frame: u8,
    pub zero: u8,
    pub pmin: u8,
    pub psec: u8,
    pub pframe: u8,
}

/// How the sectors of an extent are stored in a payload file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layout {
    Cooked2048,
    Mode2_2336,
    Raw2352,
    /// Raw sectors with 96 bytes of interleaved subchannel appended.
    Raw2352Sub96,
}

impl Layout {
    pub fn stride(self) -> usize {
        match self {
            Layout::Cooked2048 => 2048,
            Layout::Mode2_2336 => 2336,
            Layout::Raw2352 => 2352,
            Layout::Raw2352Sub96 => 2448,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    File {
        file: usize,
        offset: u64,
        layout: Layout,
        /// Audio stored big-endian (`MOTOROLA`): swap sample bytes.
        swap: bool,
    },
    /// Audio pregap / postgap: digital silence.
    Silence,
    /// Data pregap / postgap: zero-filled sectors of the track's mode with valid EDC/ECC.
    ZeroData,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SubSource {
    /// 96 bytes per sector, deinterleaved P..W (CloneCD `.sub`).
    File { file: usize, offset: u64 },
}

/// A contiguous run of sectors with one storage.
#[derive(Debug, Clone, Copy)]
pub struct Extent {
    pub lba: i32,
    pub count: u32,
    pub source: Source,
    pub sub: Option<SubSource>,
}

impl Extent {
    pub fn end(&self) -> i32 {
        self.lba + self.count as i32
    }
}

#[derive(Debug, Clone)]
pub struct Track {
    /// 1..=99, unique across sessions.
    pub number: u8,
    pub mode: TrackMode,
    /// Q control nibble: audio 0x0 (|1 pre-emphasis, |2 DCP, |8 4ch), data 0x4.
    pub control: u8,
    pub isrc: Option<[u8; 12]>,
    /// (index number, absolute LBA of its first sector), ascending; index 1 always present.
    pub indices: Vec<(u8, i32)>,
    /// Index 1.
    pub start_lba: i32,
    /// Exclusive.
    pub end_lba: i32,
    /// Contiguous coverage of `[first index .. end_lba)`.
    pub extents: Vec<Extent>,
}

impl Track {
    /// First sector of the track (index 0 when present, else index 1).
    pub fn first_lba(&self) -> i32 {
        self.indices.first().map(|i| i.1).unwrap_or(self.start_lba)
    }
    /// Index number the sector at `lba` belongs to (the caller knows it is inside the track).
    pub fn index_at(&self, lba: i32) -> u8 {
        let mut idx = self.indices[0].0;
        for &(n, start) in &self.indices {
            if lba >= start {
                idx = n;
            }
        }
        idx
    }
    pub fn index_start(&self, index: u8) -> Option<i32> {
        self.indices.iter().find(|i| i.0 == index).map(|i| i.1)
    }
}

#[derive(Debug, Clone)]
pub struct Session {
    pub number: u8,
    pub tracks: Vec<Track>,
    pub leadout_lba: i32,
}

/// An opened payload file (bin / img / sub / wav), read by offset.
pub struct Payload {
    pub path: PathBuf,
    file: File,
    pub len: u64,
}

impl Payload {
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileExt;
            self.file
                .read_exact_at(buf, offset)
                .map_err(|e| Error::Io(format!("{}: {} bytes at {}: {}", self.path.display(), buf.len(), offset, e)))
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::FileExt;
            let mut done = 0;
            while done < buf.len() {
                let n = self
                    .file
                    .seek_read(&mut buf[done..], offset + done as u64)
                    .map_err(|e| Error::Io(format!("{}: {}", self.path.display(), e)))?;
                if n == 0 {
                    return Err(Error::Io(format!("{}: short read at {}", self.path.display(), offset)));
                }
                done += n;
            }
            Ok(())
        }
    }
}

/// The disc.
pub struct Disc {
    /// 1-based session numbers, ascending.
    pub sessions: Vec<Session>,
    /// CATALOG, 13 ASCII digits.
    pub mcn: Option<[u8; 13]>,
    /// CloneCD `[Entry]` records verbatim; `None` = synthesize the raw TOC.
    pub raw_toc: Option<Vec<TocEntry>>,
    files: Vec<Payload>,
}

impl fmt::Debug for Disc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Disc")
            .field("sessions", &self.sessions)
            .field("mcn", &self.mcn)
            .field("raw_toc", &self.raw_toc.as_ref().map(|t| t.len()))
            .field("files", &self.files.iter().map(|p| p.path.display().to_string()).collect::<Vec<_>>())
            .finish()
    }
}

impl Disc {
    pub(crate) fn new() -> Disc {
        Disc { sessions: Vec::new(), mcn: None, raw_toc: None, files: Vec::new() }
    }

    /// Open `path` as a payload file; returns its index for `Source::File`.
    pub(crate) fn add_file(&mut self, path: &Path) -> Result<usize> {
        let file = File::open(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
        let len = file.metadata()?.len();
        self.files.push(Payload { path: path.to_path_buf(), file, len });
        Ok(self.files.len() - 1)
    }

    pub fn file(&self, index: usize) -> &Payload {
        &self.files[index]
    }

    /// Open an image: `.cue`, `.ccd`, `.iso`, dispatching on the
    /// extension first and on the content when the extension says nothing.
    pub fn open(path: &Path) -> Result<Disc> {
        let ext = path.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).unwrap_or_default();
        match ext.as_str() {
            "cue" => cue::open(path),
            "ccd" => ccd::open(path),
            "mds" => mds::open(path),
            "iso" => iso::open(path),
            _ => {
                let mut head = [0u8; 4096];
                let n = {
                    use std::io::Read;
                    let mut f = File::open(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
                    let mut got = 0;
                    while got < head.len() {
                        match f.read(&mut head[got..]) {
                            Ok(0) => break,
                            Ok(k) => got += k,
                            Err(e) => return Err(e.into()),
                        }
                    }
                    got
                };
                match sniff(&head[..n]) {
                    Some("cue") => cue::open(path),
                    Some("ccd") => ccd::open(path),
                    Some("mds") => mds::open(path),
                    Some("iso") => iso::open(path),
                    _ => Err(Error::Invalid(format!("{}: not a disc image libdisc reads", path.display()))),
                }
            }
        }
    }

    /// Lead-out LBA of the last session: the number of addressable sectors.
    pub fn sector_count(&self) -> u32 {
        self.sessions.last().map(|s| s.leadout_lba as u32).unwrap_or(0)
    }

    pub fn track_count(&self) -> u8 {
        self.sessions.iter().map(|s| s.tracks.len() as u8).sum()
    }

    pub fn tracks(&self) -> impl Iterator<Item = (&Session, &Track)> {
        self.sessions.iter().flat_map(|s| s.tracks.iter().map(move |t| (s, t)))
    }

    pub fn track(&self, number: u8) -> Option<(&Session, &Track)> {
        self.tracks().find(|(_, t)| t.number == number)
    }

    pub fn first_track(&self) -> u8 {
        self.tracks().next().map(|(_, t)| t.number).unwrap_or(1)
    }

    pub fn last_track(&self) -> u8 {
        self.tracks().last().map(|(_, t)| t.number).unwrap_or(1)
    }

    fn check_range(&self, lba: i32) -> Result<()> {
        if lba < 0 || lba >= self.sector_count() as i32 {
            Err(Error::Range)
        } else {
            Ok(())
        }
    }

    /// The session and track covering `lba`, with the index number; `None`
    /// inside a gap between sessions.
    pub fn locate(&self, lba: i32) -> Option<(&Session, &Track, u8)> {
        for s in &self.sessions {
            for t in &s.tracks {
                if lba >= t.first_lba() && lba < t.end_lba {
                    return Some((s, t, t.index_at(lba)));
                }
            }
        }
        None
    }

    /// The next track after the one covering `lba` (for the P-channel pause flag).
    fn track_after(&self, number: u8) -> Option<&Track> {
        let mut it = self.tracks().skip_while(|(_, t)| t.number != number);
        it.next();
        it.next().map(|(_, t)| t)
    }

    fn extent_at<'a>(&self, t: &'a Track, lba: i32) -> Option<&'a Extent> {
        t.extents.iter().find(|e| lba >= e.lba && lba < e.end())
    }

    /// The 2352 raw bytes of the sector at `lba`: stored, or synthesized
    /// from cooked data (with EDC/ECC), silence or zero data.
    pub fn read_raw(&self, lba: i32, out: &mut [u8; 2352]) -> Result<()> {
        self.check_range(lba)?;
        let Some((_, t, _)) = self.locate(lba) else {
            out.fill(0);
            return Ok(());
        };
        let e = self.extent_at(t, lba).ok_or_else(|| Error::Invalid(format!("track {} has no extent at LBA {lba}", t.number)))?;
        match e.source {
            Source::Silence => out.fill(0),
            Source::ZeroData => synth_from_cooked(t.mode, lba, &[0u8; 2048], out),
            Source::File { file, offset, layout, swap } => {
                let stride = layout.stride();
                let off = offset + (lba - e.lba) as u64 * stride as u64;
                match layout {
                    Layout::Raw2352 | Layout::Raw2352Sub96 => {
                        self.files[file].read_at(off, out)?;
                        if swap {
                            for p in out.chunks_exact_mut(2) {
                                p.swap(0, 1);
                            }
                        }
                    }
                    Layout::Cooked2048 => {
                        let mut data = [0u8; 2048];
                        self.files[file].read_at(off, &mut data)?;
                        synth_from_cooked(t.mode, lba, &data, out);
                    }
                    Layout::Mode2_2336 => {
                        let mut body = [0u8; 2336];
                        self.files[file].read_at(off, &mut body)?;
                        sector::build_mode2_2336(lba, &body, out);
                    }
                }
            }
        }
        Ok(())
    }

    /// The kind of the sector at `lba` without verifying L-EC (reads the
    /// sector only for tracks whose form is per sector).
    pub fn classify(&self, lba: i32) -> Result<(SectorKind, u8, u8)> {
        self.check_range(lba)?;
        let Some((_, t, index)) = self.locate(lba) else {
            return Ok((SectorKind::Gap, 0, 0));
        };
        let e = self.extent_at(t, lba).ok_or_else(|| Error::Invalid(format!("track {} has no extent at LBA {lba}", t.number)))?;
        let stored_raw = matches!(e.source, Source::File { layout: Layout::Raw2352 | Layout::Raw2352Sub96 | Layout::Mode2_2336, .. });
        let kind = if t.mode == TrackMode::Audio || !stored_raw {
            t.mode.default_kind()
        } else {
            let mut raw = [0u8; 2352];
            self.read_raw(lba, &mut raw)?;
            match (t.mode, sector::detect_kind(&raw)) {
                (TrackMode::Mode2Formless, _) => SectorKind::Mode2Formless,
                (_, Some(k)) => k,
                (m, None) => m.default_kind(),
            }
        };
        Ok((kind, t.number, index))
    }

    /// Kind, track, index and L-EC state of the sector at `lba`.
    pub fn sector_info(&self, lba: i32) -> Result<SectorInfo> {
        let (kind, track, index) = self.classify(lba)?;
        let lec_ok = if kind.is_data() {
            let mut raw = [0u8; 2352];
            self.read_raw(lba, &mut raw)?;
            Some(sector::verify(&raw, kind).is_ok())
        } else {
            None
        };
        Ok(SectorInfo { kind, track, index, lec_ok })
    }

    /// The 2048 user bytes of a Mode 1 / Mode 2 form 1 sector after L-EC
    /// verification: `Err(Medium)` on a mismatch, `Err(Mode)` for audio,
    /// gap and form 2 sectors (what a drive answers to READ(10)).
    pub fn read_cooked(&self, lba: i32, out: &mut [u8; 2048]) -> Result<()> {
        let mut raw = [0u8; 2352];
        let (kind, _, _) = self.classify(lba)?;
        self.read_raw(lba, &mut raw)?;
        let kind = match kind {
            SectorKind::Mode1 | SectorKind::Mode2Form1 => kind,
            SectorKind::Audio | SectorKind::Gap | SectorKind::Mode2Form2 | SectorKind::Mode2Formless => return Err(Error::Mode),
        };
        if !sector::verify(&raw, kind).is_ok() {
            return Err(Error::Medium);
        }
        let (off, _) = sector::user_data_range(kind).unwrap();
        out.copy_from_slice(&raw[off..off + 2048]);
        Ok(())
    }

    /// The 96 subchannel bytes (deinterleaved P..W) of the sector at
    /// `lba`: stored when the extent has a `.sub`, synthesized otherwise.
    pub fn read_sub(&self, lba: i32, out: &mut [u8; 96]) -> Result<()> {
        self.check_range(lba)?;
        if let Some((_, t, _)) = self.locate(lba) {
            if let Some(e) = self.extent_at(t, lba) {
                match e.sub {
                    Some(SubSource::File { file, offset }) => {
                        let off = offset + (lba - e.lba) as u64 * 96;
                        if off + 96 <= self.files[file].len {
                            return self.files[file].read_at(off, out);
                        }
                    }
                    None => {
                        if let Source::File { file, offset, layout: Layout::Raw2352Sub96, .. } = e.source {
                            let off = offset + (lba - e.lba) as u64 * 2448 + 2352;
                            let mut inter = [0u8; 96];
                            self.files[file].read_at(off, &mut inter)?;
                            subq::deinterleave(&inter, out);
                            return Ok(());
                        }
                    }
                }
            }
        }
        subq::synthesize(self, lba, out);
        Ok(())
    }

    /// P-channel pause flag for `lba`: inside index 00, or in the 150
    /// sectors before the next track's index 01.
    pub(crate) fn pause_at(&self, lba: i32) -> bool {
        match self.locate(lba) {
            Some((_, t, index)) => {
                if index == 0 {
                    return true;
                }
                match self.track_after(t.number) {
                    Some(n) => n.start_lba - lba <= PREGAP_SECTORS && lba < n.start_lba,
                    None => false,
                }
            }
            None => true,
        }
    }
}

/// A raw sector of `mode` around 2048 cooked bytes.
fn synth_from_cooked(mode: TrackMode, lba: i32, data: &[u8; 2048], out: &mut [u8; 2352]) {
    match mode {
        TrackMode::Audio => {
            out.fill(0);
        }
        TrackMode::Mode1 => sector::build_mode1(lba, data, out),
        TrackMode::Mode2Form2 => {
            let mut d = [0u8; 2324];
            d[..2048].copy_from_slice(data);
            sector::build_mode2_form2(lba, &[0, 0, 0x20, 0, 0, 0, 0x20, 0], &d, out)
        }
        _ => sector::build_mode2_form1(lba, &[0u8; 8], data, out),
    }
}

/// Content sniff for `Disc::open` and `libdisc_probe`: `"cue"`, `"ccd"`,
/// `"iso"` or `None`.
pub fn sniff(head: &[u8]) -> Option<&'static str> {
    let text = String::from_utf8_lossy(head);
    let t = text.to_ascii_uppercase();
    if t.trim_start().starts_with("[CLONECD]") {
        return Some("ccd");
    }
    if head.starts_with(b"MEDIA DESCRIPTOR") {
        return Some("mds");
    }
    if t.contains("TRACK ") && t.contains("FILE ") && t.contains("INDEX ") {
        return Some("cue");
    }
    if head.len() >= 16 * 2048 + 6 && &head[16 * 2048 + 1..16 * 2048 + 6] == b"CD001" {
        return Some("iso");
    }
    None
}

/// Resolve a payload path named by an image next to `base`: as written,
/// then case-insensitively in the same directory (dumps made on Windows).
pub(crate) fn resolve_payload(base: &Path, name: &str) -> Result<PathBuf> {
    let dir = base.parent().unwrap_or_else(|| Path::new("."));
    let name = name.replace('\\', "/");
    let leaf = Path::new(&name).file_name().and_then(|s| s.to_str()).unwrap_or(&name).to_string();
    let direct = dir.join(&name);
    if direct.is_file() {
        return Ok(direct);
    }
    let direct_leaf = dir.join(&leaf);
    if direct_leaf.is_file() {
        return Ok(direct_leaf);
    }
    if let Ok(rd) = std::fs::read_dir(dir) {
        for ent in rd.flatten() {
            if ent.file_name().to_string_lossy().eq_ignore_ascii_case(&leaf) && ent.path().is_file() {
                return Ok(ent.path());
            }
        }
    }
    Err(Error::Invalid(format!("{}: payload file not found: {}", base.display(), direct.display())))
}
