use fast_hilbert::h2xy;
use std::collections::HashMap;

use crate::reader::FileStreamer;
use anyhow::Result;

const HILBERT_ORDER: u8 = 9;
pub const GRID_SIZE: u32 = 1 << HILBERT_ORDER; // 1024
const DECAY_FACTOR: f32 = 0.97;
const CHUNK_SIZE: usize = 1000;

fn addr_to_coords(addr: u64) -> (u32, u32) {
    let max_index = (1u64 << (2 * HILBERT_ORDER)) - 1;
    let index = addr & max_index;
    h2xy(index, HILBERT_ORDER)
}

fn coords_to_grid(x: u32, y: u32) -> usize {
    ((y as usize) * GRID_SIZE as usize + (x as usize)) * 4
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
    heat_map: HeatMap,
    streamer: FileStreamer,
}

impl Visualizer {
    pub fn new(log_path: &str) -> Result<Self> {
        Ok(Self {
            heat_map: HeatMap::new(DECAY_FACTOR),
            streamer: FileStreamer::new(log_path)?,
        })
    }

    pub fn process_chunk(&mut self) -> Result<bool> {
        let entries = self.streamer.read_chunk(CHUNK_SIZE)?;
        if entries.is_empty() {
            return Ok(false);
        }

        for entry in entries {
            let (x, y) = addr_to_coords(entry.data_addr);
            let is_write = entry.operation == 'w';
            self.heat_map.add_access(x, y, is_write);
        }

        Ok(true)
    }

    pub fn render(&mut self, frame: &mut [u8]) {
        // clear screen
        for pixel in frame.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[0, 0, 0, 255]);
        }

        // draw heatmap
        for ((x, y), heat) in &self.heat_map.data {
            let idx = coords_to_grid(*x, *y);
            let color = heat_to_color(*heat);
            frame[idx..idx + 4].copy_from_slice(&color);
        }

        // update heatmap
        self.heat_map.decay();
    }
}
