use fast_hilbert::h2xy;
use std::{collections::HashMap, time::Duration};

use crate::{reader::DataSource, ring::MemRef};
use color_eyre::Result;

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

#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn heat_to_color(heat: f32) -> [u8; 4] {
    let heat = heat.clamp(0.0, 1.0);

    let (r, g, b) = if heat < 0.33 {
        // black -> red
        let t = heat / 0.33;
        ((255.0 * t) as u8, 0, 0)
    } else if heat < 0.66 {
        // red -> yellow
        let t = (heat - 0.33) / 0.33;
        (255, (255.0 * t) as u8, 0)
    } else {
        // yellow -> white
        let t = (heat - 0.66) / 0.34;
        (255, 255, (255.0 * t) as u8)
    };

    [r, g, b, 255]
}

struct HeatMap {
    data: HashMap<(u32, u32), f32>,
    half_life: Duration,
}

impl HeatMap {
    fn new(half_life: Duration) -> Self {
        Self {
            data: HashMap::new(),
            half_life,
        }
    }

    fn add_access(&mut self, x: u32, y: u32, _is_write: bool) {
        self.data.insert((x, y), 1.0);
    }

    fn decay(&mut self, dt: Duration) {
        let factor = 0.5_f32.powf(dt.as_secs_f32() / self.half_life.as_secs_f32());
        self.data.retain(|_, heat| {
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

    pub fn process_chunk(&mut self) -> Result<bool> {
        self.source.poll_chunk()?.map_or_else(
            || Ok(false),
            |chunk| {
                self.apply_chunk(chunk);
                Ok(true)
            },
        )
    }

    fn apply_chunk(&mut self, chunk: Vec<MemRef>) {
        for event in chunk {
            let (x, y) = addr_to_coords(event.addr as u64);
            self.heatmap.add_access(x, y, event.write);
        }
    }

    #[allow(clippy::indexing_slicing)]
    pub fn render(&mut self, frame: &mut [u8], dt: Duration) {
        // clear screen
        for pixel in frame.as_chunks_mut::<4>().0 {
            pixel[..3].fill(0);
        }

        // draw heatmap
        for ((x, y), heat) in &self.heatmap.data {
            let idx = coords_to_grid(*x, *y);
            let color = heat_to_color(*heat);
            frame[idx..idx + 4].copy_from_slice(&color);
        }

        // update heatmap
        self.heatmap.decay(dt);
    }
}
