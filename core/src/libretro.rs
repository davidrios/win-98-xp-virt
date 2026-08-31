//! Minimal hand-written libretro FFI surface (Spike B baseline).
//!
//! Only the subset the core currently uses. If Spike B lands on an external
//! crate, this module is replaced; the exported symbols in lib.rs stay.

#![allow(non_camel_case_types, dead_code)]

use std::os::raw::{c_char, c_uint, c_void};

pub const RETRO_API_VERSION: c_uint = 1;

pub const RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: c_uint = 10;
pub const RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: c_uint = 18;

// enum retro_pixel_format: 0RGB1555 = 0, XRGB8888 = 1, RGB565 = 2
pub const RETRO_PIXEL_FORMAT_XRGB8888: c_uint = 1;

pub const RETRO_REGION_NTSC: c_uint = 0;

#[repr(C)]
pub struct retro_system_info {
    pub library_name: *const c_char,
    pub library_version: *const c_char,
    pub valid_extensions: *const c_char,
    pub need_fullpath: bool,
    pub block_extract: bool,
}

#[repr(C)]
pub struct retro_game_geometry {
    pub base_width: c_uint,
    pub base_height: c_uint,
    pub max_width: c_uint,
    pub max_height: c_uint,
    pub aspect_ratio: f32,
}

#[repr(C)]
pub struct retro_system_timing {
    pub fps: f64,
    pub sample_rate: f64,
}

#[repr(C)]
pub struct retro_system_av_info {
    pub geometry: retro_game_geometry,
    pub timing: retro_system_timing,
}

#[repr(C)]
pub struct retro_game_info {
    pub path: *const c_char,
    pub data: *const c_void,
    pub size: usize,
    pub meta: *const c_char,
}

pub type retro_environment_t = unsafe extern "C" fn(cmd: c_uint, data: *mut c_void) -> bool;
pub type retro_video_refresh_t =
    unsafe extern "C" fn(data: *const c_void, width: c_uint, height: c_uint, pitch: usize);
pub type retro_audio_sample_t = unsafe extern "C" fn(left: i16, right: i16);
pub type retro_audio_sample_batch_t =
    unsafe extern "C" fn(data: *const i16, frames: usize) -> usize;
pub type retro_input_poll_t = unsafe extern "C" fn();
pub type retro_input_state_t =
    unsafe extern "C" fn(port: c_uint, device: c_uint, index: c_uint, id: c_uint) -> i16;
