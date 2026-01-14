use anyhow::Result;
use std::{
    fs::File,
    io::{BufRead, BufReader},
};

#[derive(Debug, Clone)]
pub struct LogEntry {
    _instruction_addr: u64,
    pub operation: char,
    _data_size: u8,
    pub data_addr: u64,
}

impl LogEntry {
    fn from_line(line: &str) -> Result<Self> {
        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() != 4 {
            anyhow::bail!("invalid format");
        }

        let instruction_addr = u64::from_str_radix(parts[0].trim_start_matches("0x"), 16)?;
        let operation = parts[1].trim().chars().next().unwrap_or('r');
        let data_size = parts[2].trim().parse()?;
        let data_addr = u64::from_str_radix(parts[3].trim_start_matches("0x"), 16)?;

        Ok(LogEntry {
            _instruction_addr: instruction_addr,
            operation,
            _data_size: data_size,
            data_addr,
        })
    }
}

pub struct FileStreamer {
    reader: BufReader<File>,
}

impl FileStreamer {
    pub fn new(path: &str) -> Result<Self> {
        let file = File::open(path)?;
        let reader = BufReader::new(file);
        Ok(Self { reader })
    }

    pub fn read_chunk(&mut self, chunk_size: usize) -> Result<Vec<LogEntry>> {
        let mut entries = Vec::new();
        let mut line = String::new();

        for _ in 0..chunk_size {
            line.clear();
            let bytes_read = self.reader.read_line(&mut line)?;
            if bytes_read == 0 {
                break;
            }

            if let Ok(entry) = LogEntry::from_line(line.trim()) {
                entries.push(entry);
            }
        }

        Ok(entries)
    }
}
