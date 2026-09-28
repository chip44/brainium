use eframe::{egui, wgpu};
use std::time::{Duration, Instant};

use crate::poller::Poller;
use crate::visualizer::{GRID_SIZE, HeatGrid, Visualizer};

pub const APP_NAME: &str = "Brainium";

/// max fps to render the framebuffer in
const FPS: u64 = 15;
/// frame time of `FPS`
const FRAME_TIME: Duration = Duration::from_nanos(1_000_000_000 / FPS);

struct Stats {
    last_total_entries: u64,
    total_entries: u64,
    entries_rate: f64,
}

impl Default for Stats {
    fn default() -> Self {
        Self {
            last_total_entries: 0,
            total_entries: 0,
            entries_rate: 0.0,
        }
    }
}

pub struct App {
    /// holds raw pixel data (RGBA) for the pixel grid
    framebuffer: Vec<u8>,
    framebuffer_texture: wgpu::Texture,
    framebuffer_id: egui::TextureId,
    /// applies polled entries to the heatmap and renders it
    visualizer: Visualizer,
    /// polls entries on its own thread
    poller: Poller,
    /// last render time, used to limit fps to avoid repeated heavy computation
    last_update: Instant,
    /// entries statistics
    stats: Stats,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let render_state = cc.wgpu_render_state.as_ref().expect("wgpu renderer");

        let device = &render_state.device;

        let framebuffer_texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("framebuffer"),
            #[allow(clippy::cast_possible_truncation)]
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

        let grid = HeatGrid::new();

        Self {
            // init the whole framebuffer to fully-opaque black pixels
            framebuffer: vec![0, 0, 0, 255]
                .into_iter()
                .cycle()
                .take(GRID_SIZE * GRID_SIZE * 4)
                .collect(),
            framebuffer_texture,
            framebuffer_id,
            visualizer: Visualizer::new(grid.clone()),
            poller: Poller::spawn(grid),
            last_update: Instant::now(),
            stats: Stats::default(),
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
                bytes_per_row: u32::try_from(GRID_SIZE * 4).ok(),
                rows_per_image: u32::try_from(GRID_SIZE).ok(),
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

        self.last_update = now;

        let total_entries = self.poller.total_entries();
        let entries_num = total_entries - self.stats.last_total_entries;
        self.stats.last_total_entries = total_entries;
        self.stats.total_entries = total_entries;

        let alpha = 0.25;
        #[allow(clippy::cast_precision_loss)]
        let instant_rate = (entries_num as f64) / dt.as_secs_f64();
        let entries_rate = self.stats.entries_rate;
        self.stats.entries_rate = entries_rate.mul_add(1.0 - alpha, instant_rate * alpha);

        self.visualizer.render(&mut self.framebuffer, dt);
    }

    fn ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame) {
        if let Some(render_state) = frame.wgpu_render_state() {
            self.upload_framebuffer(&render_state.queue);
        }

        egui::Panel::left("sidebar")
            .resizable(false)
            .show(ui, |ui| {
                ui.heading("Statistics");
                ui.label(format!("{} entries", self.stats.total_entries));
                ui.label(format!("{:.0} en/s", self.stats.entries_rate));
            });

        egui::CentralPanel::default().show(ui, |ui| {
            egui::ScrollArea::both()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    #[allow(clippy::cast_precision_loss)]
                    ui.image(egui::load::SizedTexture::new(
                        self.framebuffer_id,
                        [GRID_SIZE as f32, GRID_SIZE as f32],
                    ));
                })
        });
    }
}
