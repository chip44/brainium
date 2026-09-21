use fast_hilbert::h2xy;
use std::collections::HashMap;

use crate::reader::{DataSource, MemEvent};
use color_eyre::Result;

const HILBERT_ORDER: u8 = 9;
pub const GRID_SIZE: u32 = 1 << HILBERT_ORDER; // 1024
const DECAY_FACTOR: f32 = 0.97;

fn addr_to_coords(addr: u64) -> (u32, u32) {
    let max_index = (1u64 << (2 * HILBERT_ORDER)) - 1;
    let index = addr & max_index;
    h2xy(index, HILBERT_ORDER)
}

const fn coords_to_grid(x: u32, y: u32) -> usize {
    ((y as usize) * (GRID_SIZE as usize) + (x as usize)) * 4
}

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
    decay_factor: f32,
}

impl HeatMap {
    fn new(decay_factor: f32) -> Self {
        Self {
            data: HashMap::new(),
            decay_factor,
        }
    }

    fn add_access(&mut self, x: u32, y: u32, _is_write: bool) {
        self.data.insert((x, y), 1.0);
    }

    fn decay(&mut self) {
        self.data.retain(|_, heat| {
            *heat *= self.decay_factor;
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
            heatmap: HeatMap::new(DECAY_FACTOR),
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

    fn apply_chunk(&mut self, chunk: Vec<MemEvent>) {
        for event in chunk {
            let (x, y) = addr_to_coords(event.data_addr);
            self.heatmap.add_access(x, y, event.is_write);
        }
    }

    pub fn render(&mut self, frame: &mut [u8]) {
        // clear screen
        for pixel in frame.as_chunks_mut::<4>().0 {
            pixel.copy_from_slice(&[0, 0, 0, 255]);
        }

        // draw heatmap
        for ((x, y), heat) in &self.heatmap.data {
            let idx = coords_to_grid(*x, *y);
            let color = heat_to_color(*heat);
            frame[idx..idx + 4].copy_from_slice(&color);
        }

        // update heatmap
        self.heatmap.decay();
    }
}
