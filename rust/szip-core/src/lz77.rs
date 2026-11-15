//! LZ77 hash-chain match finder with price-aware candidate selection.
//! Original implementation, twin of native/lz77.hpp — same parameters, same
//! decisions (deterministic, byte-identical output).

use crate::{MAX_MATCH, MIN_MATCH};

const WINDOW_LOG: u32 = 20;
const WINDOW: u32 = 1 << WINDOW_LOG;
const HASH_BITS: u32 = 18;
const HASH_SIZE: usize = 1 << HASH_BITS;
const MAX_CHAIN: u32 = 400;
pub const NICE_LEN: u32 = 128;

#[derive(Clone, Copy, Default)]
pub struct Match {
    pub len: u32,
    pub off: u32, // 1-based
}

pub struct MatchFinder {
    head: Vec<u32>,
    prev: Vec<u32>,
}

#[inline]
fn hash3(p: &[u8]) -> usize {
    let v = (p[0] as u32) | ((p[1] as u32) << 8) | ((p[2] as u32) << 16);
    (v.wrapping_mul(2_654_435_761) >> (32 - HASH_BITS)) as usize
}

impl MatchFinder {
    pub fn new() -> Self {
        MatchFinder {
            head: Vec::new(),
            prev: Vec::new(),
        }
    }

    /// `prev` is indexed modulo the block length; call once per block.
    pub fn prepare(&mut self, block_len: usize) {
        self.head = vec![0u32; HASH_SIZE];
        self.prev = vec![0u32; if block_len == 0 { 1 } else { block_len }];
    }

    pub fn insert(&mut self, data: &[u8], n: usize, pos: usize) {
        if pos + MIN_MATCH as usize > n {
            return;
        }
        let h = hash3(&data[pos..]);
        let prev_len = self.prev.len();
        self.prev[pos % prev_len] = self.head[h];
        self.head[h] = (pos + 1) as u32;
    }

    pub fn find(&self, data: &[u8], n: usize, pos: usize) -> Match {
        let mut best = Match::default();
        let mut best_score = i64::MIN;
        if pos + MIN_MATCH as usize > n {
            return best;
        }

        let max_len = core::cmp::min(MAX_MATCH as usize, n - pos);
        let limit = if pos > WINDOW as usize {
            pos - WINDOW as usize
        } else {
            0
        };

        let prev_len = self.prev.len();
        let mut cand = self.head[hash3(&data[pos..])];
        let mut depth = 0u32;
        while depth < MAX_CHAIN && cand != 0 {
            let mpos = (cand - 1) as usize;
            if mpos < limit {
                break;
            }
            if mpos >= pos {
                // current position (or lazy look-ahead) is in the chain: skip it
                cand = self.prev[mpos % prev_len];
                depth += 1;
                continue;
            }

            // cheap reject: first 3 bytes must match
            if data[mpos] != data[pos]
                || data[mpos + 1] != data[pos + 1]
                || data[mpos + 2] != data[pos + 2]
            {
                cand = self.prev[mpos % prev_len];
                depth += 1;
                continue;
            }

            let mut len = 3usize;
            while len < max_len && data[mpos + len] == data[pos + len] {
                len += 1;
            }

            // price-aware: prefer longer matches, but a much nearer match can win
            let off = (pos - mpos) as u32;
            let mut bl = 0u32;
            let mut t = off;
            while t != 0 {
                bl += 1;
                t >>= 1;
            }
            let score = len as i64 * 8 - bl as i64 * 3;
            if best.len == 0 || score > best_score {
                best.len = len as u32;
                best.off = off;
                best_score = score;
            }
            if len >= NICE_LEN as usize || len == max_len {
                break;
            }
            cand = self.prev[mpos % prev_len];
            depth += 1;
        }
        best
    }
}

impl Default for MatchFinder {
    fn default() -> Self {
        Self::new()
    }
}
