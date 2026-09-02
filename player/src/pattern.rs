//! M0 stand-in for the guest framebuffer: 640×480 XRGB8888 test pattern.
//! Replaced by QEMU's DisplaySurface in M1; kept afterwards as a diagnostic
//! source (shader/geometry checks without booting a guest).

pub const WIDTH: usize = 640;
pub const HEIGHT: usize = 480;

// SMPTE-ish bars so channel order / gamma mistakes are obvious at a glance.
const BARS: [u32; 7] = [
    0x00c0c0c0, 0x00c0c000, 0x0000c0c0, 0x0000c000, 0x00c000c0, 0x00c00000, 0x000000c0,
];

pub struct Pattern {
    pub fb: Vec<u32>,
    frame: u64,
}

impl Pattern {
    pub fn new() -> Self {
        Self {
            fb: vec![0; WIDTH * HEIGHT],
            frame: 0,
        }
    }

    /// Render the next frame into `fb` (XRGB8888, row-major, pitch = WIDTH).
    pub fn render(&mut self) {
        let sweep = (self.frame as usize) % HEIGHT;
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                let mut px = BARS[x * BARS.len() / WIDTH];
                // 1px white border: pixel-exact edges are visible under shaders
                if x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 {
                    px = 0x00ffffff;
                }
                // moving scanline proves cadence (one sweep per 8 s at 60 Hz)
                if y == sweep {
                    px = 0x00ffffff;
                }
                self.fb[y * WIDTH + x] = px;
            }
        }
        self.frame = self.frame.wrapping_add(1);
    }
}
