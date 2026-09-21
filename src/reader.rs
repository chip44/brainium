use color_eyre::{
    Result,
    eyre::{Context, bail},
};
use shared_memory::{Shmem, ShmemConf};
use std::{
    fs::File,
    io::{BufRead, BufReader},
    mem::ManuallyDrop,
    sync::atomic::{AtomicU64, Ordering},
};

const CHUNK_SIZE: usize = 1000;

#[repr(C, align(32))]
#[derive(Debug, Clone)]
pub struct MemEvent {
    pub is_write: bool,
    _pad: [u8; 7],
    pub data_addr: u64,
    pub data_size: u64,
    pub instr_addr: u64,
}

const _: () = assert!(std::mem::size_of::<MemEvent>() == 32);
const _: () = assert!(std::mem::align_of::<MemEvent>() == 32);

pub trait DataSource {
    /// Try to get the next chunk.
    ///
    /// - Ok(Some(chunk)): new data available
    /// - Ok(None): no data
    /// - Err(e): error
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemEvent>>>;
}

pub struct FileSource {
    reader: BufReader<File>,
}

impl FileSource {
    pub fn new(path: &str) -> Result<Self> {
        let file = File::open(path)?;
        let reader = BufReader::new(file);
        Ok(Self { reader })
    }

    fn process_line(line: &str) -> Result<MemEvent> {
        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() != 4 {
            bail!("invalid format");
        }
        let instr_addr = u64::from_str_radix(parts[0].trim_start_matches("0x"), 16)?;
        let data_size = parts[2].trim().parse()?;
        let data_addr = u64::from_str_radix(parts[3].trim_start_matches("0x"), 16)?;
        let is_write = parts[1].trim().chars().next().unwrap_or('r') != 'r';

        Ok(MemEvent {
            is_write,
            _pad: [0; 7],
            data_addr,
            data_size,
            instr_addr,
        })
    }
}

impl DataSource for FileSource {
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemEvent>>> {
        let mut events = Vec::new();
        let mut line = String::new();

        for _ in 0..CHUNK_SIZE {
            line.clear();
            let bytes_read = self.reader.read_line(&mut line)?;
            if bytes_read == 0 {
                break;
            }

            match Self::process_line(line.trim()) {
                Ok(event) => events.push(event),
                Err(e) => eprintln!("failed processing line '{}': {}", line.trim(), e),
            }
        }

        Ok((!events.is_empty()).then_some(events))
    }
}

const SHM_NAME: &str = "brainium_shm";
const SHM_RING_SIZE: usize = 1 << 20;

#[repr(C, align(128))]
struct Ring {
    write_idx: AtomicU64,
    read_idx: AtomicU64,
    dropped: AtomicU64,
    _pad: u64,
    entries: [MemEvent; SHM_RING_SIZE],
}

pub struct RingSource {
    ring_ptr: *mut Ring,
    shm: ManuallyDrop<Shmem>,
    last_dropped: u64,
}

impl RingSource {
    pub fn new() -> Result<Self> {
        let shm = ShmemConf::new()
            .os_id(SHM_NAME)
            .size(std::mem::size_of::<Ring>())
            .open()
            .context("Failed to open shared memory. Is the producer running?")?;

        // validate size
        if shm.len() < std::mem::size_of::<Ring>() {
            bail!(
                "Shared memory size mismatch: expected {}, got {}",
                std::mem::size_of::<Ring>(),
                shm.len()
            );
        }

        let ring_ptr = shm.as_ptr().cast::<Ring>();

        Ok(Self {
            ring_ptr,
            shm: ManuallyDrop::new(shm),
            last_dropped: 0,
        })
    }

    fn ring(&self) -> &Ring {
        // SAFETY: ring_ptr is valid for the lifetime of _shm
        unsafe { &*self.ring_ptr }
    }
}

impl Drop for RingSource {
    fn drop(&mut self) {
        // SAFETY: we need to manually drop the Shmem
        unsafe {
            ManuallyDrop::drop(&mut self.shm);
        }
    }
}

impl DataSource for RingSource {
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemEvent>>> {
        let dropped = {
            let ring = self.ring();
            ring.dropped.load(Ordering::Relaxed)
        };
        if dropped != self.last_dropped {
            eprintln!(
                "ring buffer overflow: writer dropped {} events since last poll",
                dropped.wrapping_sub(self.last_dropped)
            );
            self.last_dropped = dropped;
        }

        let ring = self.ring();

        let write_idx = ring.write_idx.load(Ordering::Acquire);
        let read_idx = ring.read_idx.load(Ordering::Relaxed);

        if read_idx == write_idx {
            return Ok(None);
        }

        let mut available = write_idx.wrapping_sub(read_idx);
        if available as usize > SHM_RING_SIZE {
            // writer lapped us completely, some of this range is already gone.
            // dropped counter above should reflect this, just clamp so we don't
            // re-read stale/overwritten slots.
            available = SHM_RING_SIZE as u64;
        }

        let to_read = available.min(CHUNK_SIZE as u64);
        let mut events = Vec::with_capacity(to_read as usize);

        for i in 0..to_read {
            let idx = ((read_idx.wrapping_add(i)) as usize) & (SHM_RING_SIZE - 1);
            // SAFETY: slot is within bounds; write_idx (Acquire load above)
            // guarantees the writer's data for this slot is visible.
            let event = unsafe { std::ptr::read_volatile(&ring.entries[idx]) };
            events.push(event);
        }

        ring.read_idx
            .store(read_idx.wrapping_add(to_read), Ordering::Release);

        Ok((!events.is_empty()).then_some(events))
    }
}
