use anyhow::{Result, bail};
use std::{
    fs::File,
    io::{BufRead, BufReader},
};

const CHUNK_SIZE: usize = 1000;

#[derive(Debug, Clone)]
pub struct MemEvent {
    pub instr_addr: u64,
    pub data_size: u8,
    pub data_addr: u64,
    pub is_write: bool,
}

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
            instr_addr,
            data_size,
            data_addr,
            is_write,
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
