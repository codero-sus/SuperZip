//! SZLZRC v3 block codec: LZ77 + rep offsets + context-mixing literal coder.
//! Original implementation, twin of native/codec.cpp block layer.

use crate::lz77::{MatchFinder, NICE_LEN};
use crate::rc::{RcDecoder, RcEncoder, PROB_INIT};
use crate::{MAX_MATCH, MIN_MATCH};

/// Error type shared by the engines and the container layer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SzipError {
    Io,
    Format,
    Corrupt,
    Unsupported,
    Nomem,
    Truncated,
    Buffer,
}

impl std::fmt::Display for SzipError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            SzipError::Io => "i/o error",
            SzipError::Format => "bad format",
            SzipError::Corrupt => "corrupt archive",
            SzipError::Unsupported => "unsupported feature",
            SzipError::Nomem => "out of memory",
            SzipError::Truncated => "truncated archive",
            SzipError::Buffer => "buffer too small",
        };
        f.write_str(s)
    }
}

impl std::error::Error for SzipError {}

impl SzipError {
    /// Error codes matching the C ABI in native/szip.h.
    pub fn to_code(self) -> i32 {
        match self {
            SzipError::Io => -1,
            SzipError::Format => -2,
            SzipError::Corrupt => -3,
            SzipError::Unsupported => -4,
            SzipError::Nomem => -5,
            SzipError::Truncated => -6,
            SzipError::Buffer => -7,
        }
    }

    pub fn from_code(code: i32) -> SzipError {
        match code {
            -1 => SzipError::Io,
            -2 => SzipError::Format,
            -3 => SzipError::Corrupt,
            -4 => SzipError::Unsupported,
            -5 => SzipError::Nomem,
            -6 => SzipError::Truncated,
            -7 => SzipError::Buffer,
            _ => SzipError::Corrupt,
        }
    }
}


struct Model {
    is_match: u16,
    is_rep: u16,
    rep_sel: [u16; 2],
    len_unary: [u16; 12],   // gamma of (len - 2), x in [1, 2046]
    len_bits: [u16; 10],
    off_mode: u16,          // 0: tiny (off 1-4), 1: slot+direct
    tiny_unary: [u16; 8],   // gamma of off, x in [1, 4]
    tiny_bits: [u16; 8],
    slot_unary: [u16; 12],  // gamma of (n - 2), x in [1, 18]
    slot_bits: [u16; 10],
    off_direct: [u16; 19],  // low n-1 bits of off-1
}

impl Model {
    fn new() -> Self {
        Model {
            is_match: PROB_INIT,
            is_rep: PROB_INIT,
            rep_sel: [PROB_INIT; 2],
            len_unary: [PROB_INIT; 12],
            len_bits: [PROB_INIT; 10],
            off_mode: PROB_INIT,
            tiny_unary: [PROB_INIT; 8],
            tiny_bits: [PROB_INIT; 8],
            slot_unary: [PROB_INIT; 12],
            slot_bits: [PROB_INIT; 10],
            off_direct: [PROB_INIT; 19],
        }
    }

}

/// Elias gamma for x >= 1: n one-bits, one zero-bit, then n low bits MSB first.
fn encode_gamma(rc: &mut RcEncoder, unary: &mut [u16], bits: &mut [u16], x: u32) {
    let mut n = 0u32;
    while (2u32 << n) <= x {
        n += 1;
    }
    for i in 0..n {
        rc.encode_bit(&mut unary[i as usize], 1);
    }
    rc.encode_bit(&mut unary[n as usize], 0);
    let mut i = n;
    while i > 0 {
        i -= 1;
        let b = (x >> i) & 1;
        rc.encode_bit(&mut bits[i as usize], b);
    }
}

fn decode_gamma(
    rc: &mut RcDecoder,
    unary: &mut [u16],
    bits: &mut [u16],
    n_max: u32,
) -> Result<u32, SzipError> {
    let mut n = 0u32;
    while rc.decode_bit(&mut unary[n as usize]) == 1 {
        n += 1;
        if n > n_max {
            return Err(SzipError::Corrupt);
        }
    }
    let mut x = 1u32;
    let mut i = n;
    while i > 0 {
        i -= 1;
        x = (x << 1) | rc.decode_bit(&mut bits[i as usize]);
    }
    Ok(x)
}

// Offset: u = off - 1, n = bitlen(u).
//   tiny mode (off 1-4):  gamma(off)
//   slot mode (off >= 5): gamma(n - 2), then n-1 direct low bits of u
fn encode_offset(rc: &mut RcEncoder, m: &mut Model, off: u32) {
    let u = off - 1;
    if u < 4 {
        rc.encode_bit(&mut m.off_mode, 0);
        encode_gamma(rc, &mut m.tiny_unary, &mut m.tiny_bits, off);
        return;
    }
    rc.encode_bit(&mut m.off_mode, 1);
    let mut n = 0u32;
    while (1u32 << n) <= u {
        n += 1;
    }
    encode_gamma(rc, &mut m.slot_unary, &mut m.slot_bits, n - 2);
    let mut i = n - 1;
    while i > 0 {
        i -= 1;
        let b = (u >> i) & 1;
        rc.encode_bit(&mut m.off_direct[i as usize], b);
    }
}

fn decode_offset(rc: &mut RcDecoder, m: &mut Model) -> Result<u32, SzipError> {
    if rc.decode_bit(&mut m.off_mode) == 0 {
        let off = decode_gamma(rc, &mut m.tiny_unary, &mut m.tiny_bits, 6)?;
        if off == 0 || off > 4 {
            return Err(SzipError::Corrupt);
        }
        return Ok(off);
    }
    let x = decode_gamma(rc, &mut m.slot_unary, &mut m.slot_bits, 10)?;
    if x < 1 || x > 18 {
        return Err(SzipError::Corrupt);
    }
    let n = x + 2;
    let mut u = 1u32 << (n - 1);
    let mut i = n - 1;
    while i > 0 {
        i -= 1;
        u |= rc.decode_bit(&mut m.off_direct[i as usize]) << i;
    }
    if u + 1 > crate::MAX_OFFSET {
        return Err(SzipError::Corrupt);
    }
    Ok(u + 1)
}

struct RepPick {
    len: u32,
    idx: u32,
}

fn rep_match(src: &[u8], n: usize, pos: usize, reps: &[u32; 3]) -> RepPick {
    let mut best = RepPick { len: 0, idx: 0 };
    let max_len = core::cmp::min(MAX_MATCH as usize, n - pos);
    for i in 0..3 {
        let off = reps[i] as usize;
        if off == 0 || off > pos {
            continue;
        }
        let mut len = 0usize;
        while len < max_len && src[pos + len] == src[pos - off + len] {
            len += 1;
        }
        if len >= MIN_MATCH as usize && len as u32 > best.len {
            best.len = len as u32;
            best.idx = i as u32;
        }
    }
    best
}

fn reps_after_normal(reps: &mut [u32; 3], off: u32) {
    reps[2] = reps[1];
    reps[1] = reps[0];
    reps[0] = off;
}

fn reps_after_rep(reps: &mut [u32; 3], idx: u32) {
    if idx > 0 {
        let tmp = reps[idx as usize];
        let mut j = idx;
        while j > 0 {
            reps[j as usize] = reps[(j - 1) as usize];
            j -= 1;
        }
        reps[0] = tmp;
    }
}

/// bit price of Elias gamma(x): 2*floor(log2 x) + 1
fn price_gamma(x: u32) -> u32 {
    let mut g = 0u32;
    while (2u32 << g) <= x {
        g += 1;
    }
    2 * g + 1
}

/// Compress one block with a given parse bias into `out` (cleared first).
fn encode_block_szlzrc(src: &[u8], out: &mut Vec<u8>, parse_bias: u32) {
    out.clear();
    let n = src.len();
    let mut rc = RcEncoder::new();
    let mut m = Model::new();
    let mut mix = MixLit::new();
    let mut hist = Hist::new();
    let mut mf = MatchFinder::new();
    mf.prepare(n);

    let mut inserted = vec![false; n];
    let mut pos = 0usize;
    let mut reps = [0u32; 3];

    while pos < n {
        if !inserted[pos] {
            inserted[pos] = true;
            mf.insert(src, n, pos);
        }
        let rep = rep_match(src, n, pos, &reps);
        let nm = mf.find(src, n, pos);

        let mut use_rep = false;
        if rep.len >= MIN_MATCH {
            if nm.len < MIN_MATCH || rep.len > nm.len {
                use_rep = true;
            } else {
                let rep_cost = 3 + price_gamma(rep.len - 2);
                let nm_cost = price_gamma(nm.off) + price_gamma(nm.len - 2);
                use_rep = rep_cost + 5 * (nm.len - rep.len) < nm_cost;
            }
        }
        let cur_len = if use_rep { rep.len } else { nm.len };

        // reject matches that cost more than writing literals
        let mut eff_len = cur_len;
        if cur_len >= MIN_MATCH {
            let est = if use_rep {
                3 + price_gamma(cur_len - 2)
            } else {
                1 + price_gamma(nm.off) + price_gamma(cur_len - 2)
            };
            let lit_price = mix.peek_span(&hist, src, pos, cur_len as usize);
            if est * parse_bias > lit_price {
                eff_len = 0;
            }
        }

        if eff_len >= MIN_MATCH {
            let mut skip = false;
            if eff_len < NICE_LEN && pos + 1 < n {
                if !inserted[pos + 1] {
                    inserted[pos + 1] = true;
                    mf.insert(src, n, pos + 1);
                }
                let rep2 = rep_match(src, n, pos + 1, &reps);
                let nm2 = mf.find(src, n, pos + 1);
                let next_len = core::cmp::max(rep2.len, nm2.len);
                if next_len >= eff_len + 1 {
                    skip = true;
                }
            }
            if skip {
                rc.encode_bit(&mut m.is_match, 0);
                mix.encode(&mut rc, &mut hist, src[pos]);
                pos += 1;
                continue;
            }

            if use_rep {
                rc.encode_bit(&mut m.is_match, 1);
                rc.encode_bit(&mut m.is_rep, 1);
                rc.encode_bit(&mut m.rep_sel[0], if rep.idx == 0 { 0 } else { 1 });
                if rep.idx != 0 {
                    rc.encode_bit(&mut m.rep_sel[1], if rep.idx == 1 { 0 } else { 1 });
                }
                encode_gamma(&mut rc, &mut m.len_unary, &mut m.len_bits, rep.len - 2);
                reps_after_rep(&mut reps, rep.idx);
                for p in pos + 1..pos + rep.len as usize {
                    if !inserted[p] {
                        inserted[p] = true;
                        mf.insert(src, n, p);
                    }
                }
                for i in pos..pos + rep.len as usize {
                    mix.train_byte(&mut hist, src[i]);
                }
                pos += rep.len as usize;
            } else {
                rc.encode_bit(&mut m.is_match, 1);
                rc.encode_bit(&mut m.is_rep, 0);
                encode_offset(&mut rc, &mut m, nm.off);
                encode_gamma(&mut rc, &mut m.len_unary, &mut m.len_bits, nm.len - 2);
                reps_after_normal(&mut reps, nm.off);
                for p in pos + 1..pos + nm.len as usize {
                    if !inserted[p] {
                        inserted[p] = true;
                        mf.insert(src, n, p);
                    }
                }
                for i in pos..pos + nm.len as usize {
                    mix.train_byte(&mut hist, src[i]);
                }
                pos += nm.len as usize;
            }
        } else {
            rc.encode_bit(&mut m.is_match, 0);
            mix.encode(&mut rc, &mut hist, src[pos]);
            pos += 1;
        }
    }
    rc.flush();
    *out = rc.into_bytes();
}

/// Compress one block into `out` (cleared first). Encoder-side multi-parse:
/// try several match/literal price biases and keep the smallest payload.
pub fn compress_block(src: &[u8], out: &mut Vec<u8>) -> Result<(), SzipError> {
    out.clear();
    let mut best: Vec<u8> = Vec::new();
    let mut cand: Vec<u8> = Vec::new();
    for bias in [13u32, 16, 20] {
        encode_block_szlzrc(src, &mut cand, bias);
        if best.is_empty() || cand.len() < best.len() {
            core::mem::swap(&mut best, &mut cand);
        }
    }
    *out = best;
    Ok(())
}

/// Decompress one SZLZRC block into `dst` (length = raw size).
pub fn decompress_block(src: &[u8], dst: &mut [u8]) -> Result<(), SzipError> {
    let n = dst.len();
    if n == 0 {
        return Ok(());
    }
    if src.len() < 5 {
        return Err(SzipError::Truncated);
    }
    let mut rc = RcDecoder::new(src);
    if !rc.init() {
        return Err(SzipError::Truncated);
    }

    let mut m = Model::new();
    let mut mix = MixLit::new();
    let mut hist = Hist::new();
    let mut out_pos = 0usize;
    let mut reps = [0u32; 3];

    while out_pos < n {
        if rc.decode_bit(&mut m.is_match) == 0 {
            dst[out_pos] = mix.decode(&mut rc, &hist);
            hist.push(dst[out_pos]);
            out_pos += 1;
        } else if rc.decode_bit(&mut m.is_rep) == 1 {
            let mut idx = 0u32;
            if rc.decode_bit(&mut m.rep_sel[0]) == 1 {
                idx = if rc.decode_bit(&mut m.rep_sel[1]) == 0 { 1 } else { 2 };
            }
            let xlen = decode_gamma(&mut rc, &mut m.len_unary, &mut m.len_bits, 10)?;
            if !rc.ok() {
                return Err(SzipError::Truncated);
            }
            let len = xlen + 2;
            if len < MIN_MATCH || len > MAX_MATCH {
                return Err(SzipError::Corrupt);
            }
            let off = reps[idx as usize];
            if off == 0 || off as usize > out_pos {
                return Err(SzipError::Corrupt);
            }
            if len as usize > n - out_pos {
                return Err(SzipError::Corrupt);
            }
            let off = off as usize;
            for i in 0..len as usize {
                let b = dst[out_pos - off + i];
                dst[out_pos + i] = b;
            }
            reps_after_rep(&mut reps, idx);
            for i in 0..len as usize {
                let b = dst[out_pos + i];
                mix.train_byte(&mut hist, b);
            }
            out_pos += len as usize;
        } else {
            let off = decode_offset(&mut rc, &mut m)?;
            let xlen = decode_gamma(&mut rc, &mut m.len_unary, &mut m.len_bits, 10)?;
            if !rc.ok() {
                return Err(SzipError::Truncated);
            }
            let len = xlen + 2;
            if len < MIN_MATCH || len > MAX_MATCH {
                return Err(SzipError::Corrupt);
            }
            if off == 0 || off as usize > out_pos {
                return Err(SzipError::Corrupt);
            }
            if len as usize > n - out_pos {
                return Err(SzipError::Corrupt);
            }
            let off = off as usize;
            for i in 0..len as usize {
                let b = dst[out_pos - off + i];
                dst[out_pos + i] = b;
            }
            reps_after_normal(&mut reps, off as u32);
            for i in 0..len as usize {
                let b = dst[out_pos + i];
                mix.train_byte(&mut hist, b);
            }
            out_pos += len as usize;
        }
        if !rc.ok() {
            return Err(SzipError::Truncated);
        }
    }
    if !rc.ok() {
        return Err(SzipError::Truncated);
    }
    if rc.pos() != src.len() {
        return Err(SzipError::Corrupt);
    }
    Ok(())
}

/// Upper bound for the compressed size of `raw_len` bytes (C ABI parity).
pub fn compress_bound(raw_len: usize) -> usize {
    raw_len + raw_len / 2 + 64
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(src: &[u8]) {
        let mut comp = Vec::new();
        compress_block(src, &mut comp).unwrap();
        let mut dec = vec![0u8; src.len()];
        decompress_block(&comp, &mut dec).unwrap();
        assert_eq!(src, &dec[..], "roundtrip mismatch for len {}", src.len());
    }

    #[test]
    fn patterns() {
        for n in 0..40 {
            let src: Vec<u8> = (0..n).map(|i| (i * 7 + 3) as u8).collect();
            roundtrip(&src);
        }
        roundtrip(&vec![0u8; 100000]);
        let mut v = Vec::new();
        for _ in 0..2000 {
            v.extend_from_slice(b"the quick brown fox jumps over the lazy dog. ");
        }
        roundtrip(&v);
    }

    #[test]
    fn randomish() {
        let mut state = 0x12345678u32;
        let mut rng = move || {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            state
        };
        for _ in 0..60 {
            let n = (rng() % 20000) as usize;
            let src: Vec<u8> = (0..n).map(|_| rng() as u8).collect();
            roundtrip(&src);
        }
    }

    #[test]
    fn corruption_rejected() {
        let src: Vec<u8> = (0..5000)
            .map(|i| if i % 37 == 0 { b'Z' } else { (i & 0xF) as u8 })
            .collect();
        let mut comp = Vec::new();
        compress_block(&src, &mut comp).unwrap();
        let mut bad = comp.clone();
        bad[bad.len() / 2] ^= 0xFF;
        let mut dec = vec![0u8; src.len()];
        let _ = decompress_block(&bad, &mut dec);
        for cut in [1usize, 2, 5, comp.len() / 2, comp.len() - 1] {
            if cut < comp.len() {
                let mut dec = vec![0u8; src.len()];
                assert!(decompress_block(&comp[..cut], &mut dec).is_err());
            }
        }
    }
}
