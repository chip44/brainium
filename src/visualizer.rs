use fast_hilbert::h2xy;
use palette::{Hsl, IntoColor, Srgb};
use std::{
    sync::{
        Arc,
        atomic::{AtomicU32, Ordering},
    },
    time::Duration,
};

const HILBERT_ORDER: u8 = 10;
pub const GRID_SIZE: usize = 1 << HILBERT_ORDER;
const GRID_MASK: u64 = (1u64 << (2 * HILBERT_ORDER)) - 1;

/// each grid cell represents one 4KB page.
/// nearby pages end up on nearby cells, while addresses in the same page collapse onto the same cell.
const PAGE_SHIFT: u32 = 12; // 2^12 = 4096 bytes per cell

pub fn addr_to_coords(addr: u64) -> (u32, u32) {
    let page = addr >> PAGE_SHIFT;
    let index = page & GRID_MASK;
    h2xy(index, HILBERT_ORDER)
}

/// heat is stored as a signed f32: magnitude is intensity (0.0..=1.0),
/// sign is the access type (write=negative read=positive).
fn heat_to_color(signed_heat: f32) -> [u8; 3] {
    let write = signed_heat.is_sign_negative();
    let heat = signed_heat.abs().clamp(0.0, 1.0);
    let hue = if write { 0.0 } else { 220.0 }; // write=red read=blue

    let hsl = Hsl::new(hue, 1.0, heat);
    let rgb: Srgb<u8> = Srgb::from_linear(hsl.into_color());

    [rgb.red, rgb.green, rgb.blue]
}

pub struct HeatGrid {
    /// grid of bit-cast f32 (to store heat + access type) so both the poller and ui thread can
    /// read/write lock-free
    cells: Vec<AtomicU32>,
}

impl HeatGrid {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            cells: (0..GRID_SIZE * GRID_SIZE)
                .map(|_| AtomicU32::new(0f32.to_bits()))
                .collect(),
        })
    }

    #[inline]
    pub fn add_access(&self, x: u32, y: u32, write: bool) {
        let idx = (y as usize) * GRID_SIZE + (x as usize);
        let value: f32 = if write { -1.0 } else { 1.0 };
        self.cells[idx].store(value.to_bits(), Ordering::Relaxed);
    }
}

pub struct Visualizer {
    grid: Arc<HeatGrid>,
    half_life: Duration,
}

impl Visualizer {
    pub const fn new(grid: Arc<HeatGrid>) -> Self {
        Self {
            grid,
            half_life: Duration::from_secs(2),
        }
    }

    /// render the grid onto the frame buffer
    pub fn render(&self, frame: &mut [u8], dt: Duration) {
        let factor = 0.5_f32.powf(dt.as_secs_f32() / self.half_life.as_secs_f32());
        let pixels = frame.as_chunks_mut::<4>().0;

        for (cell, pixel) in self.grid.cells.iter().zip(pixels) {
            let mut heat = f32::from_bits(cell.load(Ordering::Relaxed));

            // decay
            heat *= factor;

            if heat.abs() < 0.01 {
                // clear if fully-decayed
                heat = 0.0;
                pixel[..3].fill(0);
            } else {
                // update color
                pixel[..3].copy_from_slice(&heat_to_color(heat));
            }
            cell.store(heat.to_bits(), Ordering::Relaxed);
        }
    }
}
