//! win98-xp-virt libretro core.
//!
//! M0 state: no QEMU yet. The core loads contentless, renders a 640×480
//! test pattern at 60 fps and outputs silent audio — enough to validate the
//! export surface, the frontend handshake, and CI on all platforms. QEMU
//! embedding lands in M1 behind `libqemu_embed.h`.

mod libretro;

use libretro::*;
use std::os::raw::{c_char, c_uint, c_void};
use std::ptr;
use std::sync::Mutex;

const WIDTH: usize = 640;
const HEIGHT: usize = 480;
const FPS: f64 = 60.0;
const SAMPLE_RATE: f64 = 44100.0;
const AUDIO_FRAMES_PER_TICK: usize = (44100 / 60) as usize;

#[derive(Default)]
struct Core {
    video_cb: Option<retro_video_refresh_t>,
    audio_batch_cb: Option<retro_audio_sample_batch_t>,
    input_poll_cb: Option<retro_input_poll_t>,
    environ_cb: Option<retro_environment_t>,
    frame: u64,
    fb: Vec<u32>,
}

static CORE: Mutex<Core> = Mutex::new(Core {
    video_cb: None,
    audio_batch_cb: None,
    input_poll_cb: None,
    environ_cb: None,
    frame: 0,
    fb: Vec::new(),
});

// SMPTE-ish bars so channel order / gamma mistakes are obvious at a glance.
const BARS: [u32; 7] = [
    0x00c0c0c0, 0x00c0c000, 0x0000c0c0, 0x0000c000, 0x00c000c0, 0x00c00000, 0x000000c0,
];

fn render(core: &mut Core) {
    if core.fb.len() != WIDTH * HEIGHT {
        core.fb = vec![0; WIDTH * HEIGHT];
    }
    let sweep = (core.frame as usize) % HEIGHT;
    for y in 0..HEIGHT {
        for x in 0..WIDTH {
            let mut px = BARS[x * BARS.len() / WIDTH];
            // 1px white border: pixel-exact edges are visible under shaders
            if x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 {
                px = 0x00ffffff;
            }
            // moving scanline proves cadence (one full sweep per 8 seconds)
            if y == sweep {
                px = 0x00ffffff;
            }
            core.fb[y * WIDTH + x] = px;
        }
    }
    core.frame = core.frame.wrapping_add(1);
}

#[no_mangle]
pub extern "C" fn retro_api_version() -> c_uint {
    RETRO_API_VERSION
}

#[no_mangle]
pub extern "C" fn retro_set_environment(cb: retro_environment_t) {
    let mut core = CORE.lock().unwrap();
    core.environ_cb = Some(cb);
    let mut no_game = true;
    unsafe {
        cb(
            RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME,
            &mut no_game as *mut bool as *mut c_void,
        );
    }
}

#[no_mangle]
pub extern "C" fn retro_set_video_refresh(cb: retro_video_refresh_t) {
    CORE.lock().unwrap().video_cb = Some(cb);
}

#[no_mangle]
pub extern "C" fn retro_set_audio_sample(_cb: retro_audio_sample_t) {}

#[no_mangle]
pub extern "C" fn retro_set_audio_sample_batch(cb: retro_audio_sample_batch_t) {
    CORE.lock().unwrap().audio_batch_cb = Some(cb);
}

#[no_mangle]
pub extern "C" fn retro_set_input_poll(cb: retro_input_poll_t) {
    CORE.lock().unwrap().input_poll_cb = Some(cb);
}

#[no_mangle]
pub extern "C" fn retro_set_input_state(_cb: retro_input_state_t) {}

#[no_mangle]
pub extern "C" fn retro_init() {}

#[no_mangle]
pub extern "C" fn retro_deinit() {
    let mut core = CORE.lock().unwrap();
    *core = Core::default();
}

#[no_mangle]
/// # Safety
/// `info` must be a valid pointer to writable [`retro_system_info`]
/// (guaranteed by the libretro contract).
pub unsafe extern "C" fn retro_get_system_info(info: *mut retro_system_info) {
    unsafe {
        (*info) = retro_system_info {
            library_name: c"win98-xp-virt".as_ptr() as *const c_char,
            library_version: c"0.0.1-m0".as_ptr() as *const c_char,
            valid_extensions: c"toml".as_ptr() as *const c_char,
            need_fullpath: true,
            block_extract: false,
        };
    }
}

#[no_mangle]
/// # Safety
/// `info` must be a valid pointer to writable [`retro_system_av_info`]
/// (guaranteed by the libretro contract).
pub unsafe extern "C" fn retro_get_system_av_info(info: *mut retro_system_av_info) {
    unsafe {
        (*info) = retro_system_av_info {
            geometry: retro_game_geometry {
                base_width: WIDTH as c_uint,
                base_height: HEIGHT as c_uint,
                // ceiling for guest SVGA modes; revisited when QEMU lands
                max_width: 1600,
                max_height: 1200,
                aspect_ratio: 4.0 / 3.0,
            },
            timing: retro_system_timing {
                fps: FPS,
                sample_rate: SAMPLE_RATE,
            },
        };
    }
}

#[no_mangle]
pub extern "C" fn retro_set_controller_port_device(_port: c_uint, _device: c_uint) {}

#[no_mangle]
pub extern "C" fn retro_reset() {
    CORE.lock().unwrap().frame = 0;
}

#[no_mangle]
pub extern "C" fn retro_run() {
    let mut core = CORE.lock().unwrap();
    if let Some(poll) = core.input_poll_cb {
        unsafe { poll() };
    }
    render(&mut core);
    if let Some(video) = core.video_cb {
        unsafe {
            video(
                core.fb.as_ptr() as *const c_void,
                WIDTH as c_uint,
                HEIGHT as c_uint,
                WIDTH * 4,
            );
        }
    }
    if let Some(audio) = core.audio_batch_cb {
        let silence = [0i16; AUDIO_FRAMES_PER_TICK * 2];
        unsafe {
            audio(silence.as_ptr(), AUDIO_FRAMES_PER_TICK);
        }
    }
}

#[no_mangle]
pub extern "C" fn retro_serialize_size() -> usize {
    0
}

#[no_mangle]
pub extern "C" fn retro_serialize(_data: *mut c_void, _size: usize) -> bool {
    false
}

#[no_mangle]
pub extern "C" fn retro_unserialize(_data: *const c_void, _size: usize) -> bool {
    false
}

#[no_mangle]
pub extern "C" fn retro_cheat_reset() {}

#[no_mangle]
pub extern "C" fn retro_cheat_set(_index: c_uint, _enabled: bool, _code: *const c_char) {}

#[no_mangle]
pub extern "C" fn retro_load_game(_info: *const retro_game_info) -> bool {
    let core = CORE.lock().unwrap();
    if let Some(env) = core.environ_cb {
        let mut fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        let ok = unsafe {
            env(
                RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
                &mut fmt as *mut c_uint as *mut c_void,
            )
        };
        return ok;
    }
    false
}

#[no_mangle]
pub extern "C" fn retro_load_game_special(
    _type: c_uint,
    _info: *const retro_game_info,
    _num: usize,
) -> bool {
    false
}

#[no_mangle]
pub extern "C" fn retro_unload_game() {}

#[no_mangle]
pub extern "C" fn retro_get_region() -> c_uint {
    RETRO_REGION_NTSC
}

#[no_mangle]
pub extern "C" fn retro_get_memory_data(_id: c_uint) -> *mut c_void {
    ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn retro_get_memory_size(_id: c_uint) -> usize {
    0
}
