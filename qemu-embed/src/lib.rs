//! Bindings for `embed/libqemu_embed.h`. Hand-written: the API is ours and
//! small; `api_version()` guards against header/binding drift.
//!
//! Thread contract (see the header): [`Qemu::new`], [`Qemu::run`] and drop
//! happen on one dedicated thread; display callbacks fire on that thread
//! with the BQL held and must not block; everything else is `Send + Sync`.

use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;

pub const API_VERSION: u32 = 1;
pub const FMT_XRGB8888: u32 = 1;

#[repr(C)]
pub struct RawDisplayCb {
    pub on_switch: Option<unsafe extern "C" fn(*mut c_void, *const u8, c_int, c_int, c_int, u32)>,
    pub on_update: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int, c_int)>,
    pub on_refresh_done: Option<unsafe extern "C" fn(*mut c_void)>,
    pub on_cursor:
        Option<unsafe extern "C" fn(*mut c_void, *const u32, c_int, c_int, c_int, c_int)>,
    pub on_mouse_set: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, bool)>,
}

#[repr(C)]
pub struct qemu_embed_t {
    _private: [u8; 0],
}

extern "C" {
    fn qemu_embed_api_version() -> u32;
    fn qemu_embed_new(
        argc: c_int,
        argv: *mut *mut c_char,
        cb: *const RawDisplayCb,
        ud: *mut c_void,
    ) -> *mut qemu_embed_t;
    fn qemu_embed_run(e: *mut qemu_embed_t) -> c_int;
    fn qemu_embed_destroy(e: *mut qemu_embed_t, status: c_int);
    fn qemu_embed_vm_start(e: *mut qemu_embed_t);
    fn qemu_embed_vm_pause(e: *mut qemu_embed_t);
    fn qemu_embed_vm_reset(e: *mut qemu_embed_t);
    fn qemu_embed_vm_powerdown(e: *mut qemu_embed_t);
    fn qemu_embed_vm_shutdown(e: *mut qemu_embed_t);
    fn qemu_embed_vm_running(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_key(e: *mut qemu_embed_t, qcode: u32, down: bool);
    fn qemu_embed_atset1_to_qcode(atset1: u32) -> u32;
    fn qemu_embed_mouse_rel(e: *mut qemu_embed_t, dx: c_int, dy: c_int);
    fn qemu_embed_mouse_abs(e: *mut qemu_embed_t, x: c_int, y: c_int, w: c_int, h: c_int);
    fn qemu_embed_mouse_btn(e: *mut qemu_embed_t, button: u32, down: bool);
    fn qemu_embed_mouse_is_absolute(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_input_flush(e: *mut qemu_embed_t);
}

/// Library API version; must equal [`API_VERSION`].
pub fn api_version() -> u32 {
    unsafe { qemu_embed_api_version() }
}

/// AT set-1 scancode (0xE0-prefixed as 0xE0xx) → QKeyCode, 0 if unmapped.
pub fn atset1_to_qcode(atset1: u32) -> u32 {
    unsafe { qemu_embed_atset1_to_qcode(atset1) }
}

/// Handle to the in-process VM. Cheap to copy; the underlying object lives
/// until [`Qemu::run`] returns and the owner calls [`Qemu::destroy`].
#[derive(Clone, Copy)]
pub struct Qemu(*mut qemu_embed_t);

// The C side is documented thread-safe for everything except new/run/destroy,
// which the owning thread performs via the `Owner` token.
unsafe impl Send for Qemu {}
unsafe impl Sync for Qemu {}

/// Proof that the caller is on the QEMU thread; only it can run/destroy.
pub struct Owner(Qemu);

impl Qemu {
    /// Initialize QEMU on the *current* thread. `args` is a qemu-system
    /// argument list without argv[0]. `cb`/`ud` receive display callbacks.
    ///
    /// # Safety
    /// `ud` must stay valid until `destroy`; callbacks must uphold the header
    /// contract. Fatal configuration errors terminate the process (QEMU).
    pub unsafe fn new(
        args: &[String],
        cb: &RawDisplayCb,
        ud: *mut c_void,
    ) -> Option<(Qemu, Owner)> {
        assert_eq!(
            api_version(),
            API_VERSION,
            "libqemu-embed API version mismatch"
        );
        let mut cargs: Vec<CString> = Vec::with_capacity(args.len() + 1);
        cargs.push(CString::new("qemu-system-i386").unwrap());
        for a in args {
            cargs.push(CString::new(a.as_str()).ok()?);
        }
        let mut argv: Vec<*mut c_char> = cargs.iter().map(|c| c.as_ptr() as *mut c_char).collect();
        argv.push(ptr::null_mut());
        let e = qemu_embed_new(cargs.len() as c_int, argv.as_mut_ptr(), cb, ud);
        if e.is_null() {
            return None;
        }
        let q = Qemu(e);
        Some((q, Owner(q)))
    }

    pub fn vm_start(&self) {
        unsafe { qemu_embed_vm_start(self.0) }
    }
    pub fn vm_pause(&self) {
        unsafe { qemu_embed_vm_pause(self.0) }
    }
    pub fn vm_reset(&self) {
        unsafe { qemu_embed_vm_reset(self.0) }
    }
    pub fn vm_powerdown(&self) {
        unsafe { qemu_embed_vm_powerdown(self.0) }
    }
    pub fn vm_shutdown(&self) {
        unsafe { qemu_embed_vm_shutdown(self.0) }
    }
    pub fn vm_running(&self) -> bool {
        unsafe { qemu_embed_vm_running(self.0) }
    }
    pub fn key(&self, qcode: u32, down: bool) {
        unsafe { qemu_embed_key(self.0, qcode, down) }
    }
    pub fn mouse_rel(&self, dx: i32, dy: i32) {
        unsafe { qemu_embed_mouse_rel(self.0, dx, dy) }
    }
    pub fn mouse_abs(&self, x: i32, y: i32, w: i32, h: i32) {
        unsafe { qemu_embed_mouse_abs(self.0, x, y, w, h) }
    }
    pub fn mouse_btn(&self, button: u32, down: bool) {
        unsafe { qemu_embed_mouse_btn(self.0, button, down) }
    }
    pub fn mouse_is_absolute(&self) -> bool {
        unsafe { qemu_embed_mouse_is_absolute(self.0) }
    }
    pub fn input_flush(&self) {
        unsafe { qemu_embed_input_flush(self.0) }
    }
}

impl Owner {
    /// Run the main loop on this thread; returns QEMU's exit status.
    pub fn run(&self) -> i32 {
        unsafe { qemu_embed_run(self.0 .0) }
    }
    /// Tear down (one VM per process lifetime — QEMU cleanup is partial).
    pub fn destroy(self, status: i32) {
        unsafe { qemu_embed_destroy(self.0 .0, status) }
    }
}
