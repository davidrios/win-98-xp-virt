//! librashader (RetroArch slang) filter chain on wgpu — the CRT pass of the
//! doc 03 pipeline. Input: the guest framebuffer texture (native resolution).
//! Output: an intermediate texture sized to the letterboxed viewport, which
//! the blit pass presents. Geometry stays ours; the chain sees the final
//! viewport size so its scanline/mask math is right.

use librashader::presets::ShaderFeatures;
use librashader::runtime::wgpu::{FilterChain, FilterChainOptions, WgpuOutputView};
use librashader::runtime::{Size, Viewport};
use std::path::Path;

pub struct Chain {
    chain: FilterChain,
    frame_count: usize,
    out: Option<(wgpu::Texture, wgpu::TextureView, u32, u32)>,
    format: wgpu::TextureFormat,
}

impl Chain {
    pub fn load(
        path: &Path,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        adapter_info: wgpu::AdapterInfo,
        format: wgpu::TextureFormat,
    ) -> Result<Chain, String> {
        let opts = FilterChainOptions {
            force_no_mipmaps: false,
            enable_cache: false, // needs Features::PIPELINE_CACHE; not requested
            adapter_info: Some(adapter_info),
        };
        let chain =
            FilterChain::load_from_path(path, ShaderFeatures::NONE, device, queue, Some(&opts))
                .map_err(|e| format!("{e}"))?;
        Ok(Chain {
            chain,
            frame_count: 0,
            out: None,
            format,
        })
    }

    fn ensure_out(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        if matches!(&self.out, Some((_, _, ow, oh)) if *ow == w && *oh == h) {
            return;
        }
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("shader output"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: self.format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.out = Some((tex, view, w, h));
    }

    /// Run the chain: guest texture → output texture of size (w, h).
    /// Returns the output texture (valid until the next size change).
    pub fn run(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        input: &wgpu::Texture,
        w: u32,
        h: u32,
    ) -> Result<&wgpu::Texture, String> {
        self.ensure_out(device, w, h);
        let (tex, view, _, _) = self.out.as_ref().unwrap();
        let size = Size::new(w, h);
        let viewport = Viewport {
            x: 0.0,
            y: 0.0,
            mvp: None,
            output: WgpuOutputView::new_from_raw(view, size, self.format),
            size,
        };
        self.chain
            .frame(input, &viewport, encoder, self.frame_count, None)
            .map_err(|e| format!("{e}"))?;
        self.frame_count += 1;
        Ok(tex)
    }

    pub fn frame_count(&self) -> usize {
        self.frame_count
    }
}

/// Debug readback of a texture to PNG (PLAYER_DUMP_OUT). Blocks on the GPU.
pub fn dump_texture(device: &wgpu::Device, queue: &wgpu::Queue, tex: &wgpu::Texture, path: &str) {
    let (w, h) = (tex.width(), tex.height());
    let bpr = (w * 4).div_ceil(256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: (bpr * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("readback"),
    });
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: tex,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &buf,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(bpr),
                rows_per_image: Some(h),
            },
        },
        wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
    );
    queue.submit(Some(enc.finish()));
    let slice = buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    let _ = device.poll(wgpu::PollType::wait_indefinitely());
    rx.recv().unwrap().expect("map readback");
    let data = slice.get_mapped_range().expect("mapped range");
    let bgra = tex.format() == wgpu::TextureFormat::Bgra8Unorm
        || tex.format() == wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut rgb = Vec::with_capacity((w * h * 3) as usize);
    for y in 0..h {
        let row = &data[(y * bpr) as usize..(y * bpr + w * 4) as usize];
        for px in row.chunks(4) {
            if bgra {
                rgb.extend_from_slice(&[px[2], px[1], px[0]]);
            } else {
                rgb.extend_from_slice(&[px[0], px[1], px[2]]);
            }
        }
    }
    drop(data);
    buf.unmap();
    let file = std::fs::File::create(path).expect("dump file");
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().unwrap().write_image_data(&rgb).unwrap();
    eprintln!("dumped shader output {w}x{h} to {path}");
}
