//! Host audio output: an SPSC byte ring fed by QEMU's `embed` audiodev
//! (S16LE interleaved at the rate we tell QEMU) drained by a cpal stream.

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;

pub const RING_BYTES: usize = 1 << 16; // 64 KiB ≈ 340 ms of 48 kHz stereo s16

pub struct Ring {
    pub buf: Box<[u8]>,
    pub wr: AtomicU32, // written by QEMU
    pub rd: AtomicU32, // written by us
}

impl Ring {
    pub fn new() -> Arc<Ring> {
        Arc::new(Ring {
            buf: vec![0u8; RING_BYTES].into_boxed_slice(),
            wr: AtomicU32::new(0),
            rd: AtomicU32::new(0),
        })
    }
    fn available(&self) -> usize {
        let wr = self.wr.load(Ordering::Acquire) as usize;
        let rd = self.rd.load(Ordering::Relaxed) as usize;
        (wr.wrapping_sub(rd)) & (RING_BYTES - 1)
    }
    /// Pop up to `out.len()` s16 samples; pads with silence when starved.
    fn pop_s16(&self, out: &mut [i16]) -> usize {
        let mut rd = self.rd.load(Ordering::Relaxed) as usize;
        let avail = self.available() / 2;
        let n = avail.min(out.len());
        for s in out.iter_mut().take(n) {
            *s = i16::from_le_bytes([self.buf[rd], self.buf[(rd + 1) & (RING_BYTES - 1)]]);
            rd = (rd + 2) & (RING_BYTES - 1);
        }
        self.rd.store(rd as u32, Ordering::Release);
        for s in out.iter_mut().skip(n) {
            *s = 0;
        }
        n
    }
}

pub struct Output {
    _stream: Option<cpal::Stream>,
    pub sample_rate: u32,
}

/// Open the default output device. Returns the rate/channels QEMU must be
/// configured with (we don't resample: `-audiodev embed,out.frequency=<rate>`).
pub fn start(ring: Arc<Ring>) -> Option<Output> {
    if std::env::var("PLAYER_AUDIO_NULL").is_ok() {
        // headless: keep the ring, no device; a logger shows QEMU's writes
        eprintln!("[audio] PLAYER_AUDIO_NULL: ring only, 48000 Hz");
        std::thread::spawn(move || loop {
            std::thread::sleep(std::time::Duration::from_secs(2));
            eprintln!(
                "[audio] ring wr={} rd={}",
                ring.wr.load(Ordering::Relaxed),
                ring.rd.load(Ordering::Relaxed)
            );
        });
        return Some(Output {
            _stream: None,
            sample_rate: 48000,
        });
    }
    let host = cpal::default_host();
    let device = host.default_output_device()?;
    let default = device.default_output_config().ok()?;
    let sample_rate = default.sample_rate();
    let channels: u16 = 2;
    let config = cpal::StreamConfig {
        channels,
        sample_rate,
        buffer_size: cpal::BufferSize::Default,
    };
    let stream = match default.sample_format() {
        cpal::SampleFormat::I16 => device
            .build_output_stream(
                config,
                move |data: &mut [i16], _| {
                    ring.pop_s16(data);
                },
                |e| eprintln!("[audio] stream error: {e}"),
                None,
            )
            .ok()?,
        _ => {
            let mut tmp: Vec<i16> = Vec::new();
            device
                .build_output_stream(
                    config,
                    move |data: &mut [f32], _| {
                        tmp.resize(data.len(), 0);
                        ring.pop_s16(&mut tmp);
                        for (d, s) in data.iter_mut().zip(&tmp) {
                            *d = *s as f32 / 32768.0;
                        }
                    },
                    |e| eprintln!("[audio] stream error: {e}"),
                    None,
                )
                .ok()?
        }
    };
    stream.play().ok()?;
    eprintln!(
        "[audio] output {} Hz, {} ch, {:?}",
        sample_rate,
        channels,
        default.sample_format()
    );
    Some(Output {
        _stream: Some(stream),
        sample_rate,
    })
}
