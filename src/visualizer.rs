use fast_hilbert::h2xy;
use palette::{Hsl, IntoColor, Srgb};
use std::{collections::HashMap, time::Duration};

use crate::reader::DataSource;

const HILBERT_ORDER: u8 = 10;
pub const GRID_SIZE: usize = 1 << HILBERT_ORDER;
const GRID_MASK: u64 = (1u64 << (2 * HILBERT_ORDER)) - 1;

fn addr_to_coords(addr: u64) -> (u32, u32) {
    let index = addr & GRID_MASK;
    h2xy(index, HILBERT_ORDER)
}

const fn coords_to_grid(x: u32, y: u32) -> usize {
    ((y as usize) * GRID_SIZE + (x as usize)) * 4
}

fn heat_to_color(write: bool, heat: f32) -> [u8; 3] {
    let heat = heat.clamp(0.0, 1.0);
    let hue = if write { 0.0 } else { 220.0 }; // write=red read=blue

    let hsl = Hsl::new(hue, 1.0, heat);
    let rgb: Srgb<u8> = Srgb::from_linear(hsl.into_color());

    [rgb.red, rgb.green, rgb.blue]
}

struct HeatMap {
    data: HashMap<(u32, u32), (bool, f32)>,
    half_life: Duration,
}

impl HeatMap {
    fn new(half_life: Duration) -> Self {
        Self {
            data: HashMap::new(),
            half_life,
        }
    }

    fn add_access(&mut self, x: u32, y: u32, write: bool) {
        self.data.insert((x, y), (write, 1.0));
    }

    fn decay(&mut self, dt: Duration) {
        let factor = 0.5_f32.powf(dt.as_secs_f32() / self.half_life.as_secs_f32());
        self.data.retain(|_, (_, heat)| {
            *heat *= factor;
            *heat > 0.01
        });
    }
}

pub struct Visualizer {
    source: Box<dyn DataSource>,
    heatmap: HeatMap,
}

impl Visualizer {
    pub fn new(source: Box<dyn DataSource>) -> Self {
        Self {
            source,
            heatmap: HeatMap::new(Duration::from_secs(2)),
        }
    }

    pub fn process_chunk(&mut self) {
        let chunk = self.source.poll_chunk();
        for entry in chunk {
            let (x, y) = addr_to_coords(entry.addr as u64);
            self.heatmap.add_access(x, y, entry.write);
        }
    }

    #[allow(clippy::indexing_slicing)]
    pub fn render(&mut self, frame: &mut [u8], dt: Duration) {
        // clear screen
        for pixel in frame.as_chunks_mut::<4>().0 {
            pixel[..3].fill(0);
        }

        // draw heatmap
        for ((x, y), (write, heat)) in &self.heatmap.data {
            let idx = coords_to_grid(*x, *y);
            let color = heat_to_color(*write, *heat);
            frame[idx..idx + 3].copy_from_slice(&color);
        }

        // update heatmap
        self.heatmap.decay(dt);
    }
}
