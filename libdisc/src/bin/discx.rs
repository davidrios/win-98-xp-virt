//! discx — the libdisc exerciser (doc 17 §6.1).
//!
//!   discx selftest <outdir>            write synthetic images, check the model through them
//!   discx info <image>                 sessions, tracks, indices, extents
//!   discx dump <image> <what> [args]   hex of one answer: readraw <lba> | readcooked <lba> | sub <lba> | info <lba>
//!   discx convert <in.iso> <out.cue> [--audio a.wav ...]   cooked ISO → MODE1/2352 cue/bin (+ audio tracks)
//!
//! Every check prints PASS/FAIL with a name; the exit code is 1 on any FAIL.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use libdisc::msf::Msf;
use libdisc::{sector, subq, Disc, Error, SectorKind, TrackMode};

const DATA_SECTORS: i32 = 2000;
const T2_PREGAP: i32 = 150; // in the file (INDEX 00)
const T2_LEN: i32 = 3000;
const T3_PREGAP: i32 = 150; // PREGAP, not in the file
const T3_LEN: i32 = 1500;
const MCN: &str = "1234567890123";
const ISRC: &str = "USABC0912345";

struct Checks {
    fails: u32,
    passes: u32,
}

impl Checks {
    fn report(&mut self, name: &str, r: Result<(), String>) {
        match r {
            Ok(()) => {
                self.passes += 1;
                println!("PASS {name}");
            }
            Err(e) => {
                self.fails += 1;
                println!("FAIL {name}: {e}");
            }
        }
    }
}

fn xorshift(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

/// The 2000 × 2048 user bytes of track 1, deterministic.
fn user_data() -> Vec<u8> {
    let mut s = 0x1234_5678u32;
    let mut v = vec![0u8; DATA_SECTORS as usize * 2048];
    for chunk in v.chunks_exact_mut(4) {
        chunk.copy_from_slice(&xorshift(&mut s).to_le_bytes());
    }
    // a recognisable primary volume descriptor at sector 16 so sniffing works
    let pvd = &mut v[16 * 2048..17 * 2048];
    pvd[0] = 1;
    pvd[1..6].copy_from_slice(b"CD001");
    pvd[6] = 1;
    v
}

/// `count` audio sectors of a `freq` Hz tone, phase continuous from `start_sector`.
fn tone(freq: f64, start_sector: i64, count: i32, out: &mut Vec<u8>) {
    for s in 0..count as i64 {
        for i in 0..588i64 {
            let n = (start_sector + s) * 588 + i;
            let v = (8000.0 * (2.0 * std::f64::consts::PI * freq * n as f64 / 44100.0).sin()) as i16;
            out.extend_from_slice(&v.to_le_bytes());
            out.extend_from_slice(&v.to_le_bytes());
        }
    }
}

fn msf_str(lba_file: i32) -> String {
    let m = Msf::from_lba(lba_file - libdisc::msf::MSF_OFFSET);
    format!("{:02}:{:02}:{:02}", m.m, m.s, m.f)
}

/// Write the synthetic images; returns the raw track-1 sectors (for the checks).
fn generate(dir: &Path) -> Result<Vec<u8>, String> {
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let data = user_data();
    let mut raw1 = vec![0u8; DATA_SECTORS as usize * 2352];
    for lba in 0..DATA_SECTORS {
        let mut sec = [0u8; 2352];
        sector::build_mode1(lba, &data[lba as usize * 2048..(lba as usize + 1) * 2048], &mut sec);
        raw1[lba as usize * 2352..(lba as usize + 1) * 2352].copy_from_slice(&sec);
    }
    let mut audio = Vec::with_capacity((T2_PREGAP + T2_LEN + T3_LEN) as usize * 2352);
    audio.resize(T2_PREGAP as usize * 2352, 0);
    tone(1000.0, 0, T2_LEN, &mut audio);
    tone(440.0, 0, T3_LEN, &mut audio);

    // 1. mixed.cue/.bin
    let mut bin = raw1.clone();
    bin.extend_from_slice(&audio);
    fs::write(dir.join("mixed.bin"), &bin).map_err(|e| e.to_string())?;
    let t2_i0 = DATA_SECTORS;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_file = t2_i1 + T2_LEN;
    let cue = format!(
        "CATALOG {MCN}\nFILE \"mixed.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    ISRC {ISRC}\n    INDEX 00 {}\n    INDEX 01 {}\n  TRACK 03 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n",
        msf_str(t2_i0),
        msf_str(t2_i1),
        msf_str(t3_file)
    );
    fs::write(dir.join("mixed.cue"), cue).map_err(|e| e.to_string())?;

    // 3. cooked.cue/.bin: track 1 as MODE1/2048 in the same file
    let mut cbin = data.clone();
    cbin.extend_from_slice(&audio);
    fs::write(dir.join("cooked.bin"), &cbin).map_err(|e| e.to_string())?;
    let cue = format!(
        "CATALOG {MCN}\nFILE \"cooked.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    ISRC {ISRC}\n    INDEX 00 {}\n    INDEX 01 {}\n  TRACK 03 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n",
        msf_str(t2_i0),
        msf_str(t2_i1),
        msf_str(t3_file)
    );
    fs::write(dir.join("cooked.cue"), cue).map_err(|e| e.to_string())?;

    // 4. plain.iso
    fs::write(dir.join("plain.iso"), &data).map_err(|e| e.to_string())?;

    // 2. mixed.ccd/.img/.sub (read back from step 3 on; written from the model now)
    let disc = Disc::open(&dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let mut img = Vec::with_capacity(n as usize * 2352);
    let mut sub = Vec::with_capacity(n as usize * 96);
    for lba in 0..n {
        let mut r = [0u8; 2352];
        let mut s = [0u8; 96];
        disc.read_raw(lba, &mut r).map_err(|e| e.to_string())?;
        disc.read_sub(lba, &mut s).map_err(|e| e.to_string())?;
        img.extend_from_slice(&r);
        sub.extend_from_slice(&s);
    }
    fs::write(dir.join("mixed.img"), &img).map_err(|e| e.to_string())?;
    fs::write(dir.join("mixed.sub"), &sub).map_err(|e| e.to_string())?;
    let mut ccd = String::new();
    let session = &disc.sessions[0];
    let entries: Vec<(u8, u8, u8, i32)> = {
        let mut v = vec![
            (0xA0, 1, 0x4, (1 << 16)), // PMIN = first track, PSEC = disc type 0
            (0xA1, 1, 0x4, (session.tracks.len() as i32) << 16),
            (0xA2, 1, 0x0, session.leadout_lba),
        ];
        for t in &session.tracks {
            v.push((t.number, 1, t.control, t.start_lba));
        }
        v
    };
    ccd.push_str("[CloneCD]\nVersion=3\n[Disc]\n");
    ccd.push_str(&format!("TocEntries={}\nSessions=1\nDataTracksScrambled=0\nCDTextLength=0\nCATALOG={MCN}\n", entries.len()));
    ccd.push_str("[Session 1]\nPreGapMode=2\nPreGapSubC=1\n");
    for (i, &(point, adr, control, p)) in entries.iter().enumerate() {
        ccd.push_str(&format!("[Entry {i}]\nSession=1\nPoint=0x{point:02x}\nADR=0x{adr:02x}\nControl=0x{control:02x}\nTrackNo=0\nAMin=0\nASec=0\nAFrame=0\nALBA=-150\nZero=0\n"));
        if point >= 0xA0 && point != 0xA2 {
            ccd.push_str(&format!("PMin={}\nPSec={}\nPFrame=0\nPLBA=0\n", p >> 16, (p >> 8) & 0xFF));
        } else {
            let m = Msf::from_lba(p);
            ccd.push_str(&format!("PMin={}\nPSec={}\nPFrame={}\nPLBA={}\n", m.m, m.s, m.f, p));
        }
    }
    for t in &session.tracks {
        ccd.push_str(&format!("[TRACK {}]\nMODE={}\n", t.number, if t.mode.is_data() { 1 } else { 0 }));
        for &(i, lba) in &t.indices {
            ccd.push_str(&format!("INDEX {i}={lba}\n"));
        }
        if let Some(isrc) = t.isrc {
            ccd.push_str(&format!("ISRC={}\n", String::from_utf8_lossy(&isrc)));
        }
    }
    fs::write(dir.join("mixed.ccd"), ccd).map_err(|e| e.to_string())?;
    Ok(raw1)
}

fn expect<T: PartialEq + std::fmt::Debug>(what: &str, got: T, want: T) -> Result<(), String> {
    if got == want {
        Ok(())
    } else {
        Err(format!("{what}: got {got:?}, want {want:?}"))
    }
}

fn check_msf() -> Result<(), String> {
    expect("00:02:00", Msf { m: 0, s: 2, f: 0 }.to_lba(), 0)?;
    expect("lba 0", Msf::from_lba(0), Msf { m: 0, s: 2, f: 0 })?;
    for lba in [-150, -1, 0, 1, 74, 75, 4500, 333_000, 449_849] {
        expect(&format!("round trip {lba}"), Msf::from_lba(lba).to_lba(), lba)?;
    }
    expect("74:00:00", Msf { m: 74, s: 0, f: 0 }.to_lba(), 332_850)?;
    expect("00:00:00", Msf { m: 0, s: 0, f: 0 }.to_lba(), -150)?;
    expect("bcd", sector::bcd(59), 0x59)?;
    expect("header lba 16", sector::header(16, 1), [0x00, 0x02, 0x16, 0x01])?;
    Ok(())
}

fn check_raw_synth(dir: &Path, raw1: &[u8]) -> Result<(), String> {
    let mixed = Disc::open(&dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let cooked = Disc::open(&dir.join("cooked.cue")).map_err(|e| e.to_string())?;
    let plain = Disc::open(&dir.join("plain.iso")).map_err(|e| e.to_string())?;
    let data = user_data();
    let mut r = [0u8; 2352];
    let mut c = [0u8; 2048];
    for lba in 0..DATA_SECTORS {
        let want = &raw1[lba as usize * 2352..(lba as usize + 1) * 2352];
        cooked.read_raw(lba, &mut r).map_err(|e| format!("cooked read_raw {lba}: {e}"))?;
        if r[..] != *want {
            return Err(format!("cooked.cue synthesized sector {lba} differs from the stored one"));
        }
        plain.read_raw(lba, &mut r).map_err(|e| format!("iso read_raw {lba}: {e}"))?;
        if r[..] != *want {
            return Err(format!("plain.iso synthesized sector {lba} differs from the stored one"));
        }
        mixed.read_cooked(lba, &mut c).map_err(|e| format!("mixed read_cooked {lba}: {e}"))?;
        if c[..] != data[lba as usize * 2048..(lba as usize + 1) * 2048] {
            return Err(format!("mixed.cue cooked sector {lba} differs from the user data"));
        }
    }
    // sync, header, non-zero EDC / P / Q on a synthesized sector
    plain.read_raw(16, &mut r).map_err(|e| e.to_string())?;
    expect("sync", r[..12].to_vec(), sector::SYNC.to_vec())?;
    expect("header", r[12..16].to_vec(), vec![0x00, 0x02, 0x16, 0x01])?;
    if r[2064..2068] == [0, 0, 0, 0] || r[2076..2248].iter().all(|&b| b == 0) || r[2248..].iter().all(|&b| b == 0) {
        return Err("EDC / P / Q of sector 16 are zero".into());
    }
    // audio sectors through both images are identical
    let n = mixed.sector_count() as i32;
    let mut r2 = [0u8; 2352];
    for lba in (DATA_SECTORS..n).step_by(97) {
        mixed.read_raw(lba, &mut r).map_err(|e| e.to_string())?;
        cooked.read_raw(lba, &mut r2).map_err(|e| e.to_string())?;
        if r != r2 {
            return Err(format!("audio sector {lba} differs between mixed.cue and cooked.cue"));
        }
    }
    Ok(())
}

fn check_lec(dir: &Path) -> Result<(), String> {
    let mut bin = fs::read(dir.join("mixed.bin")).map_err(|e| e.to_string())?;
    let at = 1000 * 2352 + 500;
    bin[at] ^= 0x5A;
    fs::write(dir.join("lec.bin"), &bin).map_err(|e| e.to_string())?;
    let cue = fs::read_to_string(dir.join("mixed.cue")).map_err(|e| e.to_string())?.replace("mixed.bin", "lec.bin");
    fs::write(dir.join("lec.cue"), cue).map_err(|e| e.to_string())?;
    let disc = Disc::open(&dir.join("lec.cue")).map_err(|e| e.to_string())?;
    let mut c = [0u8; 2048];
    let mut r = [0u8; 2352];
    match disc.read_cooked(1000, &mut c) {
        Err(Error::Medium) => {}
        other => return Err(format!("read_cooked of the flipped sector: {other:?}, want Medium")),
    }
    disc.read_raw(1000, &mut r).map_err(|e| e.to_string())?;
    expect("flipped byte", r[500], bin[at])?;
    let info = disc.sector_info(1000).map_err(|e| e.to_string())?;
    expect("sector_info kind", info.kind, SectorKind::Mode1)?;
    expect("sector_info lec", info.lec_ok, Some(false))?;
    let mut c2 = [0u8; 294];
    let bits = libdisc::ecc::c2_bits(&r, SectorKind::Mode1, &mut c2);
    if bits == 0 {
        return Err("C2 bits of the flipped sector are all clear".into());
    }
    for lba in [999, 1001] {
        disc.read_cooked(lba, &mut c).map_err(|e| format!("neighbour {lba}: {e}"))?;
        expect(&format!("neighbour {lba} lec"), disc.sector_info(lba).map_err(|e| e.to_string())?.lec_ok, Some(true))?;
    }
    // an EDC-only mismatch (byte 2064) and a parity-only mismatch (byte 2100) both fail
    for (off, name) in [(2064usize, "edc"), (2100usize, "parity")] {
        let mut bin2 = fs::read(dir.join("mixed.bin")).map_err(|e| e.to_string())?;
        bin2[1500 * 2352 + off] ^= 1;
        fs::write(dir.join("lec.bin"), &bin2).map_err(|e| e.to_string())?;
        let d = Disc::open(&dir.join("lec.cue")).map_err(|e| e.to_string())?;
        match d.read_cooked(1500, &mut c) {
            Err(Error::Medium) => {}
            other => return Err(format!("{name} mismatch: read_cooked {other:?}, want Medium")),
        }
    }
    fs::write(dir.join("lec.bin"), &bin).map_err(|e| e.to_string())?;
    Ok(())
}

fn check_edges(dir: &Path) -> Result<(), String> {
    let disc = Disc::open(&dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_i0 = t2_i1 + T2_LEN;
    let t3_i1 = t3_i0 + T3_PREGAP;
    let leadout = t3_i1 + T3_LEN;
    expect("sector_count", disc.sector_count() as i32, leadout)?;
    expect("track_count", disc.track_count(), 3)?;
    expect("mcn", disc.mcn.map(|m| String::from_utf8_lossy(&m).to_string()), Some(MCN.into()))?;
    let (_, t2) = disc.track(2).ok_or("no track 2")?;
    expect("t2 isrc", t2.isrc.map(|i| String::from_utf8_lossy(&i).to_string()), Some(ISRC.into()))?;
    expect("t2 indices", t2.indices.clone(), vec![(0, DATA_SECTORS), (1, t2_i1)])?;
    let (_, t3) = disc.track(3).ok_or("no track 3")?;
    expect("t3 indices", t3.indices.clone(), vec![(0, t3_i0), (1, t3_i1)])?;
    expect("t3 end", t3.end_lba, leadout)?;
    let k = |lba: i32| disc.sector_info(lba).map(|i| (i.kind, i.track, i.index)).map_err(|e| format!("info {lba}: {e}"));
    expect("lba 0", k(0)?, (SectorKind::Mode1, 1, 1))?;
    expect("lba 1999", k(1999)?, (SectorKind::Mode1, 1, 1))?;
    expect("t2 index 0 start", k(DATA_SECTORS)?, (SectorKind::Audio, 2, 0))?;
    expect("t2 index 0 end", k(t2_i1 - 1)?, (SectorKind::Audio, 2, 0))?;
    expect("t2 index 1", k(t2_i1)?, (SectorKind::Audio, 2, 1))?;
    expect("t3 pregap start", k(t3_i0)?, (SectorKind::Audio, 3, 0))?;
    expect("t3 pregap end", k(t3_i1 - 1)?, (SectorKind::Audio, 3, 0))?;
    expect("t3 index 1", k(t3_i1)?, (SectorKind::Audio, 3, 1))?;
    expect("last", k(leadout - 1)?, (SectorKind::Audio, 3, 1))?;
    let mut r = [0u8; 2352];
    let mut c = [0u8; 2048];
    for (lba, name) in [(leadout, "lead-out"), (-1, "-1"), (u32::MAX as i32, "0xFFFFFFFF")] {
        match disc.read_raw(lba, &mut r) {
            Err(Error::Range) => {}
            other => return Err(format!("read_raw {name}: {other:?}, want Range")),
        }
        match disc.sector_info(lba) {
            Err(Error::Range) => {}
            other => return Err(format!("sector_info {name}: {other:?}, want Range")),
        }
    }
    match disc.read_cooked(DATA_SECTORS, &mut c) {
        Err(Error::Mode) => {}
        other => return Err(format!("read_cooked of an audio sector: {other:?}, want Mode")),
    }
    disc.read_raw(t3_i0 + 10, &mut r).map_err(|e| e.to_string())?;
    if r.iter().any(|&b| b != 0) {
        return Err("synthesized pregap sector is not silent".into());
    }
    disc.read_raw(t2_i1 + 10, &mut r).map_err(|e| e.to_string())?;
    if r.iter().all(|&b| b == 0) {
        return Err("tone sector is silent".into());
    }
    // a corrupt cue must be an error, not a panic
    fs::write(dir.join("bad.cue"), "FILE \"mixed.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:99\n").map_err(|e| e.to_string())?;
    match Disc::open(&dir.join("bad.cue")) {
        Err(Error::Invalid(_)) => {}
        other => return Err(format!("bad.cue: {:?}, want Invalid", other.map(|_| ()))),
    }
    fs::write(dir.join("bad2.cue"), "FILE \"nonexistent.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n").map_err(|e| e.to_string())?;
    match Disc::open(&dir.join("bad2.cue")) {
        Err(Error::Invalid(_)) => {}
        other => return Err(format!("bad2.cue: {:?}, want Invalid", other.map(|_| ()))),
    }
    Ok(())
}

fn check_subq(dir: &Path) -> Result<(), String> {
    let disc = Disc::open(&dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let stored = fs::read(dir.join("mixed.sub")).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_i0 = t2_i1 + T2_LEN;
    let t3_i1 = t3_i0 + T3_PREGAP;
    let mut samples: Vec<i32> = (0..n).step_by(37).collect();
    samples.extend([0, 1, 98, 99, 100, DATA_SECTORS - 1, DATA_SECTORS, t2_i1 - 1, t2_i1, t3_i0 - 1, t3_i0, t3_i1 - 1, t3_i1, n - 1]);
    let mut s = [0u8; 96];
    for &lba in &samples {
        disc.read_sub(lba, &mut s).map_err(|e| e.to_string())?;
        if s[..] != stored[lba as usize * 96..(lba as usize + 1) * 96] {
            return Err(format!("sub of {lba} differs from mixed.sub"));
        }
        let q = &s[12..24];
        if !subq::q_crc_ok(q) {
            return Err(format!("Q CRC of {lba} does not verify"));
        }
        let info = disc.sector_info(lba).map_err(|e| e.to_string())?;
        let (_, t) = disc.track(info.track).unwrap();
        let adr = q[0] & 0x0F;
        expect(&format!("{lba} control"), q[0] >> 4, t.control)?;
        match lba % 100 {
            98 => expect(&format!("{lba} adr"), adr, 2)?,
            99 if t.isrc.is_some() => expect(&format!("{lba} adr"), adr, 3)?,
            _ => {
                expect(&format!("{lba} adr"), adr, 1)?;
                expect(&format!("{lba} tno"), sector::from_bcd(q[1]), info.track)?;
                expect(&format!("{lba} index"), sector::from_bcd(q[2]), info.index)?;
                let abs = Msf { m: sector::from_bcd(q[7]), s: sector::from_bcd(q[8]), f: sector::from_bcd(q[9]) };
                expect(&format!("{lba} abs"), abs.to_lba(), lba)?;
                let rel = Msf { m: sector::from_bcd(q[3]), s: sector::from_bcd(q[4]), f: sector::from_bcd(q[5]) }.to_lba() + 150;
                let want = if info.index == 0 { t.start_lba - lba } else { lba - t.start_lba };
                expect(&format!("{lba} rel"), rel, want)?;
            }
        }
        let pause = info.index == 0 || disc.track(info.track + 1).map(|(_, nt)| nt.start_lba - lba <= 150).unwrap_or(false);
        expect(&format!("{lba} P"), s[0], if pause { 0xFF } else { 0 })?;
        // interleave round trip
        let mut inter = [0u8; 96];
        let mut back = [0u8; 96];
        subq::interleave(&s, &mut inter);
        subq::deinterleave(&inter, &mut back);
        if back != s {
            return Err(format!("interleave round trip differs at {lba}"));
        }
    }
    // MCN frame content
    disc.read_sub(98, &mut s).map_err(|e| e.to_string())?;
    expect("mcn packed", s[13..20].to_vec(), vec![0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x30])?;
    Ok(())
}

fn selftest(dir: &Path) -> i32 {
    let mut ck = Checks { fails: 0, passes: 0 };
    let raw1 = match generate(dir) {
        Ok(r) => r,
        Err(e) => {
            println!("FAIL generate: {e}");
            return 1;
        }
    };
    ck.report("msf", check_msf());
    ck.report("raw-synth", check_raw_synth(dir, &raw1));
    ck.report("lec", check_lec(dir));
    ck.report("edges", check_edges(dir));
    ck.report("subq-synth", check_subq(dir));
    println!("{} passed, {} failed", ck.passes, ck.fails);
    if ck.fails > 0 {
        1
    } else {
        0
    }
}

fn hex(bytes: &[u8]) {
    for (i, row) in bytes.chunks(16).enumerate() {
        print!("{:06x} ", i * 16);
        for b in row {
            print!(" {b:02x}");
        }
        println!();
    }
}

fn info(disc: &Disc) {
    println!("sectors {}  sessions {}  tracks {}", disc.sector_count(), disc.sessions.len(), disc.track_count());
    if let Some(m) = disc.mcn {
        println!("catalog {}", String::from_utf8_lossy(&m));
    }
    for s in &disc.sessions {
        println!("session {}  lead-out {}", s.number, s.leadout_lba);
        for t in &s.tracks {
            let mode = match t.mode {
                TrackMode::Audio => "audio",
                TrackMode::Mode1 => "mode1",
                TrackMode::Mode2 => "mode2",
                TrackMode::Mode2Form1 => "mode2/f1",
                TrackMode::Mode2Form2 => "mode2/f2",
                TrackMode::Mode2Formless => "cdi",
            };
            let idx: Vec<String> = t.indices.iter().map(|(i, l)| format!("{i:02}@{l}")).collect();
            print!("  track {:02} {mode:<8} ctl {:x}  [{} .. {})  indices {}", t.number, t.control, t.first_lba(), t.end_lba, idx.join(" "));
            if let Some(i) = t.isrc {
                print!("  isrc {}", String::from_utf8_lossy(&i));
            }
            println!();
            for e in &t.extents {
                println!("    extent {} +{}  {:?}{}", e.lba, e.count, e.source, if e.sub.is_some() { " +sub" } else { "" });
            }
        }
    }
}

fn dump(disc: &Disc, what: &str, args: &[String]) -> Result<(), String> {
    let lba = |i: usize| -> Result<i32, String> {
        let s = args.get(i).ok_or("missing LBA")?;
        if let Some(h) = s.strip_prefix("0x") {
            i64::from_str_radix(h, 16).map(|v| v as i32).map_err(|e| e.to_string())
        } else {
            s.parse::<i64>().map(|v| v as i32).map_err(|e| e.to_string())
        }
    };
    match what {
        "readraw" => {
            let mut r = [0u8; 2352];
            disc.read_raw(lba(0)?, &mut r).map_err(|e| e.to_string())?;
            hex(&r);
        }
        "readcooked" => {
            let mut c = [0u8; 2048];
            disc.read_cooked(lba(0)?, &mut c).map_err(|e| e.to_string())?;
            hex(&c);
        }
        "sub" => {
            let mut s = [0u8; 96];
            disc.read_sub(lba(0)?, &mut s).map_err(|e| e.to_string())?;
            hex(&s);
        }
        "info" => {
            let i = disc.sector_info(lba(0)?).map_err(|e| e.to_string())?;
            println!("{i:?}");
        }
        other => return Err(format!("unknown dump request {other}")),
    }
    Ok(())
}

/// Cooked ISO → MODE1/2352 cue/bin, plus one AUDIO track per WAVE file.
fn convert(input: &Path, out_cue: &Path, wavs: &[PathBuf]) -> Result<(), String> {
    let disc = Disc::open(input).map_err(|e| e.to_string())?;
    let stem = out_cue.file_stem().and_then(|s| s.to_str()).ok_or("bad output name")?.to_string();
    let bin_path = out_cue.with_extension("bin");
    let mut bin = fs::File::create(&bin_path).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let mut r = [0u8; 2352];
    for lba in 0..n {
        disc.read_raw(lba, &mut r).map_err(|e| format!("sector {lba}: {e}"))?;
        bin.write_all(&r).map_err(|e| e.to_string())?;
    }
    let mut cue = format!("FILE \"{stem}.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    let mut pos = n;
    for (i, w) in wavs.iter().enumerate() {
        let (off, len) = libdisc::cue::wave_data(w).map_err(|e| e.to_string())?;
        let bytes = fs::read(w).map_err(|e| format!("{}: {}", w.display(), e))?;
        let pcm = &bytes[off as usize..(off + len) as usize];
        bin.write_all(pcm).map_err(|e| e.to_string())?;
        let pad = (2352 - pcm.len() % 2352) % 2352;
        bin.write_all(&vec![0u8; pad]).map_err(|e| e.to_string())?;
        let count = ((pcm.len() + pad) / 2352) as i32;
        cue.push_str(&format!("  TRACK {:02} AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n", i + 2, msf_str(pos)));
        pos += count;
    }
    fs::write(out_cue, cue).map_err(|e| e.to_string())?;
    println!("{}: {} data sectors, {} audio tracks", out_cue.display(), n, wavs.len());
    Ok(())
}

fn usage() -> i32 {
    eprintln!("usage: discx selftest <outdir> | info <image> | dump <image> <what> [args] | convert <in.iso> <out.cue> [--audio a.wav ...]");
    2
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        std::process::exit(usage());
    }
    let rc = match args[1].as_str() {
        "selftest" => selftest(Path::new(&args[2])),
        "info" => match Disc::open(Path::new(&args[2])) {
            Ok(d) => {
                info(&d);
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "dump" if args.len() >= 4 => match Disc::open(Path::new(&args[2])) {
            Ok(d) => match dump(&d, &args[3], &args[4..]) {
                Ok(()) => 0,
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            },
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "convert" if args.len() >= 4 => {
            let mut wavs = Vec::new();
            let mut i = 4;
            while i < args.len() {
                if args[i] == "--audio" {
                    i += 1;
                } else {
                    wavs.push(PathBuf::from(&args[i]));
                    i += 1;
                }
            }
            match convert(Path::new(&args[2]), Path::new(&args[3]), &wavs) {
                Ok(()) => 0,
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            }
        }
        _ => usage(),
    };
    std::process::exit(rc);
}
