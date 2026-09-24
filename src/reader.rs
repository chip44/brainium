use color_eyre::{Result, eyre::bail};
use shared_memory::{Shmem, ShmemConf};
use std::{
    ffi::c_void,
    fs::File,
    io::{BufRead, BufReader},
    mem::ManuallyDrop,
};

use crate::ring::{MemRef, RingBuf};

const CHUNK_SIZE: usize = 1 << 13;

pub trait DataSource {
    /// Try to get the next chunk.
    ///
    /// - Ok(Some(chunk)): new data available
    /// - Ok(None): no data
    /// - Err(e): error
    #[allow(clippy::missing_errors_doc)]
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemRef>>>;
}

pub struct FileSource {
    reader: BufReader<File>,
}

impl FileSource {
    #[allow(clippy::missing_errors_doc)]
    pub fn new(path: &str) -> Result<Self> {
        let file = File::open(path)?;
        let reader = BufReader::new(file);
        Ok(Self { reader })
    }

    #[allow(clippy::indexing_slicing)]
    fn process_line(line: &str) -> Result<MemRef> {
        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() != 4 {
            bail!("invalid format");
        }

        let write = parts[1].trim().chars().next().unwrap_or('r') != 'r';
        let size = parts[2].trim().parse()?;
        let addr = u64::from_str_radix(parts[3].trim_start_matches("0x"), 16)? as *mut c_void;

        Ok(MemRef { write, addr, size })
    }
}

impl DataSource for FileSource {
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemRef>>> {
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

const SHM_NAME: &str = "/brainium_shm";

pub struct RingSource {
    shm: ManuallyDrop<Shmem>,
    ring_ptr: *mut RingBuf,
}

impl RingSource {
    #[allow(clippy::missing_errors_doc)]
    pub fn new() -> Result<Self> {
        let shm = ShmemConf::new()
            .os_id(SHM_NAME)
            .size(std::mem::size_of::<RingBuf>())
            .open()?;

        // validate size
        if shm.len() < std::mem::size_of::<RingBuf>() {
            bail!(
                "shm size mismatch: expected {}, got {}",
                std::mem::size_of::<RingBuf>(),
                shm.len()
            );
        }

        let raw_ptr = shm.as_ptr();
        if !(raw_ptr as usize).is_multiple_of(std::mem::align_of::<RingBuf>()) {
            bail!("shm is not aligned for RingBuf")
        }
        #[allow(clippy::cast_ptr_alignment)]
        let ring_ptr = raw_ptr.cast::<RingBuf>();

        Ok(Self {
            shm: ManuallyDrop::new(shm),
            ring_ptr,
        })
    }

    fn ring(&mut self) -> &mut RingBuf {
        // SAFETY: ring_ptr is valid for the lifetime of shm
        unsafe { &mut *self.ring_ptr }
    }
}

impl Drop for RingSource {
    fn drop(&mut self) {
        // SAFETY: we need to manually drop the Shmem
        unsafe { ManuallyDrop::drop(&mut self.shm) }
    }
}

impl DataSource for RingSource {
    fn poll_chunk(&mut self) -> Result<Option<Vec<MemRef>>> {
        let events = self.ring().pop_bulk(CHUNK_SIZE);
        Ok((!events.is_empty()).then_some(events))
    }
}
