use std::sync::atomic::{AtomicU64, Ordering};

pub const RING_SIZE: usize = 1 << 20;
pub const RING_MASK: usize = RING_SIZE - 1;

#[repr(C, align(32))]
#[derive(Debug, Clone)]
pub struct MemRef {
    pub write: bool,
    // NOTE: technically this should be `*mut c_void` but we only use it as u64 so in this case it's
    // easier because no unsafe is needed. this would only error on 32-bit.
    pub addr: u64,
    pub size: usize,
}

const _: () = {
    assert!(std::mem::size_of::<MemRef>() == 32);
    assert!(std::mem::align_of::<MemRef>() == 32);
};

#[repr(C, align(64))]
pub struct RingCell {
    pub seq: AtomicU64,
    pub data: MemRef,
}

const _: () = {
    assert!(std::mem::size_of::<RingCell>() == 64);
    assert!(std::mem::align_of::<RingCell>() == 64);
};

#[repr(C, align(128))]
pub struct RingBuf {
    pub buffer: [RingCell; RING_SIZE],
    pub enqueue_pos: AtomicU64,
    pub _pad: [u8; 64],
    pub dequeue_pos: u64,
}

const _: () = {
    assert!(std::mem::size_of::<RingBuf>().is_multiple_of(128));
    assert!(std::mem::offset_of!(RingBuf, buffer).is_multiple_of(32));
};

impl RingBuf {
    #[allow(
        clippy::cast_possible_wrap,
        clippy::cast_possible_truncation,
        clippy::indexing_slicing
    )]
    pub fn pop_bulk(&mut self, out: &mut Vec<MemRef>) {
        out.clear();

        let mut pos = self.dequeue_pos;

        for _ in 0..out.capacity() {
            let cell = &self.buffer[(pos as usize) & RING_MASK];

            let seq = cell.seq.load(Ordering::Acquire);

            // signed diff so sequence-number wraparound is handled correctly
            let diff = (seq as i64) - ((pos + 1) as i64);
            if diff < 0 {
                // caught up to the producers
                break;
            }

            out.push(cell.data.clone());
            cell.seq.store(pos + (RING_SIZE as u64), Ordering::Release);
            pos += 1;
        }

        self.dequeue_pos = pos;
    }
}
