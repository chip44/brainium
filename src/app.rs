use std::time::{Duration, Instant};

use eframe::egui;
use eframe::wgpu;

use crate::visualizer::{GRID_SIZE, Visualizer};

pub const APP_NAME: &str = "Brainium";

/// max fps to render the framebuffer in
const FPS: u64 = 15;
/// frame time of `FPS`
const FRAME_TIME: Duration = Duration::from_nanos(1_000_000_000 / FPS);

pub struct App {
    /// holds raw pixel data (RGBA) for the pixel grid
    framebuffer: Vec<u8>,
    framebuffer_texture: wgpu::Texture,
    framebuffer_id: egui::TextureId,
    /// responsible for fetching data and framebuffer update logic
    visualizer: Visualizer,
    /// last render time, used to limit fps to avoid repeated heavy computation
    last_update: Instant,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>, visualizer: Visualizer) -> Self {
        let render_state = cc.wgpu_render_state.as_ref().expect("wgpu renderer");

        let device = &render_state.device;

        let framebuffer_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("framebuffer"),
            size: wgpu::Extent3d {
                width: GRID_SIZE as u32,
                height: GRID_SIZE as u32,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            usage: wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::TEXTURE_BINDING,
            format: wgpu::TextureFormat::Rgba8Unorm,
            view_formats: &[],
        });

        let framebuffer_view =
            framebuffer_texture.create_view(&wgpu::TextureViewDescriptor::default());

        let framebuffer_id = render_state.renderer.write().register_native_texture(
            device,
            &framebuffer_view,
            wgpu::FilterMode::Nearest,
        );

        Self {
            // init the whole framebuffer to fully-opaque black pixels
            framebuffer: vec![0, 0, 0, 255]
                .into_iter()
                .cycle()
                .take(GRID_SIZE * GRID_SIZE * 4)
                .collect(),
            framebuffer_texture,
            framebuffer_id,
            visualizer,
            last_update: Instant::now(),
        }
    }

    /// copy the framebuffer to the GPU texture
    fn upload_framebuffer(&self, queue: &wgpu::Queue) {
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &self.framebuffer_texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &self.framebuffer,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(GRID_SIZE as u32 * 4),
                rows_per_image: Some(GRID_SIZE as u32),
            },
            self.framebuffer_texture.size(),
        );
    }
}

impl eframe::App for App {
    fn logic(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // keep repainting even when idle
        ctx.request_repaint_after(FRAME_TIME);

        let now = Instant::now();
        let dt = now.duration_since(self.last_update);

        // avoid processing too fast
        if dt < FRAME_TIME {
            return;
        }

        self.last_update += FRAME_TIME;

        self.visualizer.process_chunk();
        self.visualizer.render(&mut self.framebuffer, dt);
    }

    fn ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame) {
        if let Some(render_state) = frame.wgpu_render_state() {
            self.upload_framebuffer(&render_state.queue);
        }

        egui::Panel::left("sidebar").show(ui, |ui| {
            ui.heading(APP_NAME);
        });

        egui::CentralPanel::default().show(ui, |ui| {
            egui::ScrollArea::both()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    ui.image(egui::load::SizedTexture::new(
                        self.framebuffer_id,
                        [GRID_SIZE as f32, GRID_SIZE as f32],
                    ));
                })
        });
    }
}
