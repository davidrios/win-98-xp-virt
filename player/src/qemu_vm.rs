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
}

struct Shared {
    // current surface (valid between on_switch calls, QEMU thread only)
    surface: *const u8,
    stride: usize,
    width: usize,
    height: usize,
    back: Vec<u32>,
    // published to the render thread
    front: Frame,
}
unsafe impl Send for Shared {}

pub struct Display(Arc<Mutex<Shared>>);

impl Display {
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
    let Shared { back, front, .. } = &mut *s;
    front.pixels.copy_from_slice(back);
    front.seq += 1;
    if front.seq % 100 == 0 {
        eprintln!("[display] refresh #{}", front.seq);
    }
    crate::maybe_dump(&front.pixels, front.width, front.height, front.seq);
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
pub fn start(args: Vec<String>) -> (Qemu, Display, JoinHandle<i32>) {
    let shared = Arc::new(Mutex::new(Shared {
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
        },
    }));
    // Leak one strong ref for the C side; the process holds one VM for life.
    let ud = Arc::into_raw(shared.clone()) as usize;
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
            let (q, owner) =
                unsafe { Qemu::new(&args, &cb, ud as *mut c_void) }.expect("qemu_embed_new failed");
            q.vm_start();
            tx.send(q).unwrap();
            let status = owner.run();
            owner.destroy(status);
            status
        })
        .expect("spawn qemu thread");
    let q = rx.recv().expect("qemu thread died during init");
    (q, Display(shared), handle)
}
