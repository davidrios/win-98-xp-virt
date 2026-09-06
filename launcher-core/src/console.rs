//! The console a Windows GUI program has, or has not.
//!
//! Both front ends are *windowed* programs on Windows
//! (`windows_subsystem = "windows"`), because a console-subsystem binary
//! opens a black terminal window the moment someone double-clicks it and
//! keeps it there for the life of the launcher. That costs two things
//! back, and this module is both of them:
//!
//! * a windowed program started from `cmd.exe` inherits no console, so
//!   `launcher --paths` would print into nowhere — [`attach_parent`]
//!   borrows the console it was launched from, and only then;
//! * a windowed program has no console to *lend*, so every console
//!   program it starts gets a brand-new window of its own — one flash
//!   per `qemu-img` call and a permanent black rectangle behind the
//!   player. [`command`] is how this crate starts a subprocess, and it
//!   asks for no console at all.
//!
//! On every other platform both are nothing: `attach_parent` returns and
//! `command` is `Command::new`.

use std::path::Path;
use std::process::Command;

/// Start a subprocess without giving it a console window of its own.
///
/// Only when we have no console to lend it: run the launcher *from* a
/// terminal and its children keep inheriting that terminal, which is
/// where a developer wants the player's diagnostics. Run it from
/// Explorer and nothing flashes.
pub fn command(bin: &Path) -> Command {
    #[cfg_attr(not(windows), allow(unused_mut))]
    let mut cmd = Command::new(bin);
    #[cfg(windows)]
    if !has_console() {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// Attach to the console this process was launched from, if there is one
/// and nothing has already given us a standard output — a redirection
/// (`launcher --paths > file`, or a test harness reading a pipe) hands a
/// windowed program perfectly good handles, and those must be left
/// alone.
///
/// Call it before writing anything, and only when the command line asks
/// for output: attaching for a GUI run would put a console behind the
/// window, which is the thing this module exists to avoid.
pub fn attach_parent() {
    #[cfg(windows)]
    unsafe {
        use std::ffi::c_void;
        const ATTACH_PARENT_PROCESS: u32 = 0xFFFF_FFFF;
        const STD_OUTPUT_HANDLE: u32 = 0xFFFF_FFF5; // -11
        const STD_ERROR_HANDLE: u32 = 0xFFFF_FFF4; // -12
        const GENERIC_READ: u32 = 0x8000_0000;
        const GENERIC_WRITE: u32 = 0x4000_0000;
        const FILE_SHARE_WRITE: u32 = 2;
        const OPEN_EXISTING: u32 = 3;
        const INVALID_HANDLE_VALUE: *mut c_void = usize::MAX as *mut c_void;
        #[link(name = "kernel32")]
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
            fn GetStdHandle(which: u32) -> *mut c_void;
            fn SetStdHandle(which: u32, handle: *mut c_void) -> i32;
            fn CreateFileA(
                name: *const u8,
                access: u32,
                share: u32,
                sa: *mut c_void,
                disposition: u32,
                flags: u32,
                template: *mut c_void,
            ) -> *mut c_void;
        }
        // Redirected already: whoever started us said where output goes.
        let out = GetStdHandle(STD_OUTPUT_HANDLE);
        if !out.is_null() && out != INVALID_HANDLE_VALUE {
            return;
        }
        if AttachConsole(ATTACH_PARENT_PROCESS) == 0 {
            return; // no parent console: started from Explorer
        }
        // Attaching gives the process a console but not the three
        // handles; CONOUT$ is that console's screen buffer by name.
        let conout = CreateFileA(
            c"CONOUT$".as_ptr() as *const u8,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_WRITE,
            std::ptr::null_mut(),
            OPEN_EXISTING,
            0,
            std::ptr::null_mut(),
        );
        if conout != INVALID_HANDLE_VALUE && !conout.is_null() {
            SetStdHandle(STD_OUTPUT_HANDLE, conout);
            SetStdHandle(STD_ERROR_HANDLE, conout);
        }
    }
}

/// Whether a subprocess started by [`command`] inherits somewhere to
/// print. False only for a windowless Windows process, whose children
/// are deliberately given no console — a caller with output worth
/// keeping (the player's start-up diagnostics) redirects it to a file
/// instead of losing it.
pub fn inherits_output() -> bool {
    #[cfg(windows)]
    {
        has_console()
    }
    #[cfg(not(windows))]
    {
        true
    }
}

/// Whether this process has a console attached at all.
#[cfg(windows)]
fn has_console() -> bool {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetConsoleWindow() -> *mut c_void;
    }
    unsafe { !GetConsoleWindow().is_null() }
}
