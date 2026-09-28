use color_eyre::Result;
use std::{
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use crate::reader::{CHUNK_SIZE, DataSource, RingSource};
use crate::visualizer::{HeatGrid, addr_to_coords};

/// polls a `DataSource` on its own thread
pub struct Poller {
    total_entries: Arc<AtomicU64>,
    _handle: JoinHandle<Result>,
}

impl Poller {
    pub fn spawn(grid: Arc<HeatGrid>) -> Self {
        let total_entries = Arc::new(AtomicU64::new(0));
        let counter = Arc::clone(&total_entries);

        let handle = thread::spawn(move || -> Result {
            let mut source = RingSource::new()?;
            let mut entries = Vec::with_capacity(CHUNK_SIZE);

            let mut idle_backoff_us: u64 = 1;

            loop {
                source.poll_chunk(&mut entries);
                if entries.is_empty() {
                    // empty buffer, sleep a little instead of spinning
                    thread::sleep(Duration::from_micros(idle_backoff_us));
                    idle_backoff_us = (idle_backoff_us * 2).min(500);
                    continue;
                }
                idle_backoff_us = 1;

                counter.fetch_add(entries.len() as u64, Ordering::Relaxed);
                for entry in &entries {
                    let (x, y) = addr_to_coords(entry.addr);
                    grid.add_access(x, y, entry.write);
                }
            }
        });

        Self {
            total_entries,
            _handle: handle,
        }
    }

    pub fn total_entries(&self) -> u64 {
        self.total_entries.load(Ordering::Relaxed)
    }
}
