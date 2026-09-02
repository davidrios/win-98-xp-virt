//! In-process QEMU: spawns the QEMU thread, receives display callbacks
//! (QEMU thread, BQL held — copy and return), publishes frames to the
//! render thread.

use qemu_embed::{Qemu, RawDisplayCb};
use std::ffi::{c_int, c_void};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

pub struct Frame {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<u32>,
    pub seq: u64,
    /// when the QEMU refresh tick published this frame
    pub published: std::time::Instant,
}

/// Debug: `PLAYER_KEYS="90:enter,150:1"` presses named keys at guest frame
/// numbers (headless input verification; names: a-z, 0-9, enter, esc, space,
/// tab, up, down, left, right, f1-f12).
struct KeyScript {
    steps: Vec<(u64, Vec<u32>)>, // (frame seq, AT set-1 scancodes pressed together)
    next: usize,
}

fn key_name_to_atset1(name: &str) -> Option<u32> {
    let n = name.to_ascii_lowercase();
    let b = n.as_bytes();
    if b.len() == 1 {
        return match b[0] {
            b'a'..=b'z' => {
                const ROW: &[(u8, u32)] = &[
                    (b'q', 0x10),
                    (b'w', 0x11),
                    (b'e', 0x12),
                    (b'r', 0x13),
                    (b't', 0x14),
                    (b'y', 0x15),
                    (b'u', 0x16),
                    (b'i', 0x17),
                    (b'o', 0x18),
                    (b'p', 0x19),
                    (b'a', 0x1E),
                    (b's', 0x1F),
                    (b'd', 0x20),
                    (b'f', 0x21),
                    (b'g', 0x22),
                    (b'h', 0x23),
                    (b'j', 0x24),
                    (b'k', 0x25),
                    (b'l', 0x26),
                    (b'z', 0x2C),
                    (b'x', 0x2D),
                    (b'c', 0x2E),
                    (b'v', 0x2F),
                    (b'b', 0x30),
                    (b'n', 0x31),
                    (b'm', 0x32),
                ];
                ROW.iter().find(|(c, _)| *c == b[0]).map(|(_, s)| *s)
            }
            b'1'..=b'9' => Some(0x02 + (b[0] - b'1') as u32),
            b'0' => Some(0x0B),
            _ => None,
        };
    }
    Some(match n.as_str() {
        "ctrl" => 0x1D,
        "alt" => 0x38,
        "shift" => 0x2A,
        "enter" => 0x1C,
        "esc" => 0x01,
        "space" => 0x39,
        "tab" => 0x0F,
        "up" => 0xE048,
        "down" => 0xE050,
        "left" => 0xE04B,
        "right" => 0xE04D,
        _ => {
            let f: u32 = n.strip_prefix('f')?.parse().ok()?;
            match f {
                1..=10 => 0x3B + f - 1,
                11 => 0x57,
                12 => 0x58,
                _ => return None,
            }
        }
    })
}

fn key_script_from_env() -> Option<KeyScript> {
    let spec = std::env::var("PLAYER_KEYS").ok()?;
    let mut steps = Vec::new();
    for item in spec.split(',') {
        let (at, names) = item.split_once(':')?;
        let chord: Option<Vec<u32>> = names
            .split('+')
            .map(|n| key_name_to_atset1(n.trim()))
            .collect();
        steps.push((at.trim().parse().ok()?, chord?));
    }
    steps.sort();
    Some(KeyScript { steps, next: 0 })
}

struct Shared {
    vm: Option<Qemu>,
    script: Option<KeyScript>,
    // current surface (valid between on_switch calls, QEMU thread only)
    surface: *const u8,
    stride: usize,
    width: usize,
    height: usize,
    back: Vec<u32>,
    // published to the render thread
    front: Frame,
    // wakes the render thread after each publish (event-loop proxy)
    waker: Option<Arc<dyn Fn() + Send + Sync>>,
    // QEMU's main loop has returned; the handle must not be used any more
    stopped: bool,
    // the UI thread promises no further calls; qemu_embed_destroy may run
    released: bool,
}
unsafe impl Send for Shared {}

pub struct Display(Arc<Mutex<Shared>>);

impl Display {
    /// True once QEMU's main loop has returned (guest power-off, `quit`).
    pub fn stopped(&self) -> bool {
        self.0.lock().unwrap().stopped
    }
    /// Promise that no thread but the QEMU thread will touch the VM handle
    /// from now on; lets the QEMU thread proceed to `qemu_embed_destroy`.
    pub fn release(&self) {
        self.0.lock().unwrap().released = true;
    }
    /// Copy out the latest frame if it changed since `last_seq`.
    pub fn take_if_newer(&self, last_seq: u64) -> Option<Frame> {
        let s = self.0.lock().unwrap();
        if s.front.seq == last_seq || s.front.width == 0 {
            return None;
        }
        Some(Frame {
            width: s.front.width,
            height: s.front.height,
            pixels: s.front.pixels.clone(),
            seq: s.front.seq,
            published: s.front.published,
        })
    }
}

unsafe extern "C" fn on_switch(
    ud: *mut c_void,
    px: *const u8,
    w: c_int,
    h: c_int,
    stride: c_int,
    _fmt: u32,
) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    s.surface = px;
    s.stride = stride as usize;
    s.width = w as usize;
    s.height = h as usize;
    eprintln!("[display] switch {w}x{h} stride {stride}");
    let n = s.width * s.height;
    s.back.clear();
    s.back.resize(n, 0);
    copy_rect(&mut s, 0, 0, w as usize, h as usize);
}

unsafe extern "C" fn on_update(ud: *mut c_void, x: c_int, y: c_int, w: c_int, h: c_int) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    copy_rect(&mut s, x as usize, y as usize, w as usize, h as usize);
}

unsafe extern "C" fn on_refresh_done(ud: *mut c_void) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    if s.width == 0 {
        return;
    }
    let (w, h) = (s.width, s.height);
    if s.front.width != w || s.front.height != h {
        s.front.width = w;
        s.front.height = h;
        s.front.pixels = vec![0; w * h];
    }
    let Shared {
        back,
        front,
        vm,
        script,
        waker,
        ..
    } = &mut *s;
    front.pixels.copy_from_slice(back);
    front.seq += 1;
    front.published = std::time::Instant::now();
    let waker = waker.clone();
    if front.seq % 100 == 0 {
        eprintln!("[display] refresh #{}", front.seq);
    }
    if let (Some(vm), Some(sc)) = (vm.as_ref(), script.as_mut()) {
        while sc.next < sc.steps.len() && sc.steps[sc.next].0 <= front.seq {
            let chord = &sc.steps[sc.next].1;
            let qcodes: Vec<u32> = chord
                .iter()
                .map(|&c| qemu_embed::atset1_to_qcode(c))
                .collect();
            eprintln!(
                "[script] frame {} chord {:x?} -> qcodes {:?}",
                front.seq, chord, qcodes
            );
            for &q in &qcodes {
                vm.key(q, true);
            }
            for &q in qcodes.iter().rev() {
                vm.key(q, false);
            }
            vm.input_flush();
            sc.next += 1;
        }
    }
    crate::maybe_dump(&front.pixels, front.width, front.height, front.seq);
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

fn copy_rect(s: &mut Shared, x: usize, y: usize, w: usize, h: usize) {
    if s.surface.is_null() || w == 0 || h == 0 {
        return;
    }
    let (sw, sh) = (s.width, s.height);
    let x1 = (x + w).min(sw);
    let y1 = (y + h).min(sh);
    for row in y.min(sh)..y1 {
        // SAFETY: surface valid until the next on_switch (header contract);
        // rows are `stride` bytes apart and the rect is pre-clamped by QEMU.
        let src = unsafe { s.surface.add(row * s.stride + x * 4) as *const u32 };
        let dst = &mut s.back[row * sw + x..row * sw + x1];
        unsafe { std::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), x1 - x) };
    }
}

/// Start QEMU on its own thread. Returns once the VM is created and started.
/// `audio`: (ring, sample rate) — the ring is installed before qemu_init and
/// an `-audiodev embed,id=embed0` matching the host rate is appended; attach
/// devices with `audiodev=embed0`.
pub fn start(
    mut args: Vec<String>,
    audio: Option<(Arc<crate::audio::Ring>, u32)>,
    waker: Option<Arc<dyn Fn() + Send + Sync>>,
) -> (Qemu, Display, JoinHandle<i32>) {
    let ring_ptrs = audio.map(|(ring, rate)| {
        args.push("-audiodev".into());
        args.push(format!(
            "embed,id=embed0,out.frequency={rate},out.channels=2,out.format=s16"
        ));
        // ring lives for the process; leak a strong ref for the C side
        let r = Arc::into_raw(ring);
        (r as usize, rate)
    });
    let shared = Arc::new(Mutex::new(Shared {
        vm: None,
        script: key_script_from_env(),
        surface: std::ptr::null(),
        stride: 0,
        width: 0,
        height: 0,
        back: Vec::new(),
        front: Frame {
            width: 0,
            height: 0,
            pixels: Vec::new(),
            seq: 0,
            published: std::time::Instant::now(),
        },
        waker,
        stopped: false,
        released: false,
    }));
    // Leak one strong ref for the C side; the process holds one VM for life.
    let ud = Arc::into_raw(shared.clone()) as usize;
    let shared_q = shared.clone();
    let (tx, rx) = mpsc::channel();
    let handle = std::thread::Builder::new()
        .name("qemu".into())
        .spawn(move || {
            let cb = RawDisplayCb {
                on_switch: Some(on_switch),
                on_update: Some(on_update),
                on_refresh_done: Some(on_refresh_done),
                on_cursor: None,
                on_mouse_set: None,
            };
            if let Some((r, _)) = ring_ptrs {
                let ring = unsafe { &*(r as *const crate::audio::Ring) };
                unsafe {
                    qemu_embed::set_audio_ring(
                        ring.buf.as_ptr() as *mut u8,
                        ring.buf.len(),
                        ring.wr.as_ptr(),
                        ring.rd.as_ptr(),
                    )
                };
            }
            let (q, owner) =
                unsafe { Qemu::new(&args, &cb, ud as *mut c_void) }.expect("qemu_embed_new failed");
            q.vm_start();
            tx.send(q).unwrap();
            let status = owner.run();
            // The UI thread holds a copy of the handle (input path). Tell it
            // the loop is over and wait until it has stopped calling in;
            // destroy frees the input mutex and the object itself.
            let waker = {
                let mut s = shared_q.lock().unwrap();
                s.stopped = true;
                s.vm = None;
                s.waker.clone()
            };
            if let Some(w) = waker {
                w();
            }
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
            while !shared_q.lock().unwrap().released {
                if std::time::Instant::now() > deadline {
                    eprintln!("[qemu] UI thread did not release the VM handle; cleaning up anyway");
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            owner.destroy(status);
            status
        })
        .expect("spawn qemu thread");
    let q = rx.recv().expect("qemu thread died during init");
    // pull guest frames at ~60 Hz instead of QEMU's 30 ms default (doc 03)
    let refresh_ms: u32 = std::env::var("PLAYER_REFRESH_MS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(16);
    q.set_refresh_ms(refresh_ms);
    shared.lock().unwrap().vm = Some(q);
    (q, Display(shared), handle)
}
