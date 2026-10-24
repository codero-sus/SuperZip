//! G0D SZIP — integer context-mixing literal coder (SZLZRC v3 "GOD stage").
//! Mirrors `native/mix.hpp` exactly: deterministic fixed-point math only.

use crate::rc::{RcDecoder, RcEncoder};
use crate::tables::{BIT_PRICE, SQUASH, SQUASH_BIAS, SQUASH_SHIFT, STRETCH};

pub const W_SHIFT: u32 = 13;
pub const ERR_SHIFT: u32 = 17;
pub const TREE_SHIFT: u32 = 5;
pub const W_CLAMP: i32 = 1 << 20;
pub const W_CTX: usize = 64;
const W_INIT: i32 = (1 << W_SHIFT) / 6;

/// Rolling history of the last 5 raw bytes (b[0] = most recent).
#[derive(Clone)]
pub struct Hist {
    pub b: [u8; 5],
    pub wpos: u8,
}

impl Hist {
    pub fn new() -> Self {
        Hist {
            b: [0; 5],
            wpos: 0,
        }
    }

    pub fn push(&mut self, v: u8) {
        self.b[4] = self.b[3];
        self.b[3] = self.b[2];
        self.b[2] = self.b[1];
        self.b[1] = self.b[0];
        self.b[0] = v;
        self.wpos = if v == 32 {
            0
        } else if self.wpos < 7 {
            self.wpos + 1
        } else {
            self.wpos
        };
    }
}

fn h1(a: u8) -> u32 {
    (a as u32) >> 3
}
fn h2(a: u8, b: u8, wp: u8) -> u32 {
    ((a as u32).wrapping_mul(2654435761)
        ^ (b as u32).wrapping_mul(40503)
        ^ (wp as u32).wrapping_mul(2246822519))
        >> 19
}
fn h3(a: u8, b: u8, c: u8, wp: u8) -> u32 {
    ((a as u32).wrapping_mul(2654435761)
        ^ (b as u32).wrapping_mul(40503)
        ^ (c as u32).wrapping_mul(805459861)
        ^ (wp as u32).wrapping_mul(2246822519))
        >> (32 - 17)
}
fn h4(a: u8, b: u8, c: u8, d: u8, wp: u8) -> u32 {
    ((a as u32).wrapping_mul(2654435761)
        ^ (b as u32).wrapping_mul(40503)
        ^ (c as u32).wrapping_mul(805459861)
        ^ (d as u32).wrapping_mul(1013904223)
        ^ (wp as u32).wrapping_mul(2246822519))
        >> (32 - 17)
}
fn h5(a: u8, b: u8, c: u8, d: u8, e: u8, wp: u8) -> u32 {
    ((a as u32).wrapping_mul(2654435761)
        ^ (b as u32).wrapping_mul(40503)
        ^ (c as u32).wrapping_mul(805459861)
        ^ (d as u32).wrapping_mul(1013904223)
        ^ (e as u32).wrapping_mul(2246822519)
        ^ (wp as u32).wrapping_mul(3266489917))
        >> (32 - 17)
}
fn hw(h: &Hist) -> u32 {
    ((h.wpos as u32) << 13) | ((h.b[0] as u32) << 5) | ((h.b[1] as u32) >> 3)
}
fn wctx(bitpos: u32, h: &Hist) -> usize {
    (bitpos + 8 * ((h.b[0] as u32) >> 5)) as usize
}

fn mix_with(w: &[[i32; 6]; W_CTX], wk: usize, ps: [u16; 6]) -> u32 {
    let mut x: i64 = 0;
    for i in 0..6 {
        x += w[wk][i] as i64 * STRETCH[ps[i] as usize] as i64;
    }
    x >>= W_SHIFT;
    if x > SQUASH_BIAS as i64 {
        x = SQUASH_BIAS as i64;
    }
    if x < -(SQUASH_BIAS as i64) {
        x = -(SQUASH_BIAS as i64);
    }
    SQUASH[((x + SQUASH_BIAS as i64) >> SQUASH_SHIFT) as usize] as u32
}

fn train_with(w: &mut [[i32; 6]; W_CTX], ns: [&mut u16; 6], bit: u32, p: u32) {
    let mut xs = [0i64; 6];
    let mut ii = 0usize;
    for n in ns {
        if bit == 0 {
            *n = (*n as u32 + ((2048 - *n as u32) >> TREE_SHIFT)) as u16;
        } else {
            *n = (*n as u32 - ((*n as u32) >> TREE_SHIFT)) as u16;
        }
        xs[ii] = STRETCH[*n as usize] as i64;
        ii += 1;
    }
    let err: i32 = if bit == 0 {
        2048 - p as i32
    } else {
        -(p as i32)
    };
    for i in 0..6 {
        let mut nw = w[wk][i] + ((err as i64 * xs[i]) >> ERR_SHIFT) as i32;
        if nw > W_CLAMP {
            nw = W_CLAMP;
        }
        if nw < -W_CLAMP {
            nw = -W_CLAMP;
        }
        w[wk][i] = nw;
    }
}

pub struct MixLit {
    t: [Vec<u16>; 6],
    w: [[i32; 6]; W_CTX],
}

impl MixLit {
    pub fn new() -> Self {
        MixLit {
            t: [
                vec![1024; 32 * 256],
                vec![1024; 8192 * 256],
                vec![1024; 131072 * 256],
                vec![1024; 131072 * 256],
                vec![1024; 131072 * 256],
                vec![1024; 65536 * 256],
            ],
            w: [[W_INIT; 6]; W_CTX],
        }
    }

    #[inline]
    fn idxs(h: &Hist, sym: u32) -> [usize; 6] {
        let s = sym as usize;
        [
            h1(h.b[0]) as usize * 256 + s,
            h2(h.b[0], h.b[1], h.wpos) as usize * 256 + s,
            h3(h.b[0], h.b[1], h.b[2], h.wpos) as usize * 256 + s,
            h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos) as usize * 256 + s,
            h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) as usize * 256 + s,
            hw(h) as usize * 256 + s,
        ]
    }

    fn probs(&self, ix: [usize; 6]) -> [u16; 6] {
        [
            self.t[0][ix[0]],
            self.t[1][ix[1]],
            self.t[2][ix[2]],
            self.t[3][ix[3]],
            self.t[4][ix[4]],
            self.t[5][ix[5]],
        ]
    }

    /// Update models for one raw byte (no coding). Called for bytes covered
    /// by matches too, so prices stay calibrated.
    pub fn train_byte(&mut self, h: &mut Hist, byte: u8) {
        let mut sym: u32 = 1;
        for k in (0u32..8).rev() {
            let bit = ((byte >> k) & 1) as u32;
            let wk = wctx(7 - k, h);
            let ix = Self::idxs(h, sym);
            let p = mix_with(&self.w, wk, self.probs(ix));
            train_with(
                &mut self.w,
                [
                    &mut self.t[0][ix[0]],
                    &mut self.t[1][ix[1]],
                    &mut self.t[2][ix[2]],
                    &mut self.t[3][ix[3]],
                    &mut self.t[4][ix[4]],
                    &mut self.t[5][ix[5]],
                ],
                bit,
                p,
            );
            sym = (sym << 1) | bit;
        }
        h.push(byte);
    }

    pub fn encode(&mut self, rc: &mut RcEncoder, h: &mut Hist, byte: u8) {
        let mut sym: u32 = 1;
        for k in (0u32..8).rev() {
            let bit = ((byte >> k) & 1) as u32;
            let wk = wctx(7 - k, h);
            let ix = Self::idxs(h, sym);
            let p = mix_with(&self.w, wk, self.probs(ix));
            rc.encode_bit_p(p, bit);
            train_with(
                &mut self.w,
                [
                    &mut self.t[0][ix[0]],
                    &mut self.t[1][ix[1]],
                    &mut self.t[2][ix[2]],
                    &mut self.t[3][ix[3]],
                    &mut self.t[4][ix[4]],
                    &mut self.t[5][ix[5]],
                ],
                bit,
                p,
            );
            sym = (sym << 1) | bit;
        }
        h.push(byte);
    }

    pub fn decode(&mut self, rc: &mut RcDecoder, h: &Hist) -> u8 {
        let mut sym: u32 = 1;
        for k in 0u32..8 {
            let wk = wctx(k, h);
            let ix = Self::idxs(h, sym);
            let p = mix_with(&self.w, wk, self.probs(ix));
            let bit = rc.decode_bit_p(p);
            train_with(
                &mut self.w,
                [
                    &mut self.t[0][ix[0]],
                    &mut self.t[1][ix[1]],
                    &mut self.t[2][ix[2]],
                    &mut self.t[3][ix[3]],
                    &mut self.t[4][ix[4]],
                    &mut self.t[5][ix[5]],
                ],
                bit,
                p,
            );
            sym = (sym << 1) | bit;
        }
        (sym & 0xFF) as u8
    }

    /// Cost in 1/16 bits of coding `byte` from history `h` (no state change).
    pub fn peek(&self, h: &Hist, byte: u8) -> u32 {
        let mut sym: u32 = 1;
        let mut price: u32 = 0;
        for k in (0u32..8).rev() {
            let bit = (((byte >> k) & 1) as u32) as usize;
            let wk = wctx(7 - k, h);
            let ix = Self::idxs(h, sym);
            let p = mix_with(&self.w, wk, self.probs(ix));
            price += BIT_PRICE[if bit == 0 {
                p as usize
            } else {
                (2048 - p) as usize
            }];
            sym = (sym << 1) | bit as u32;
        }
        price
    }

    pub fn peek_span(&self, h: &Hist, src: &[u8], pos: usize, len: usize) -> u32 {
        let mut hh = h.clone();
        let mut price = 0u32;
        for i in 0..len {
            price += self.peek(&hh, src[pos + i]);
            hh.push(src[pos + i]);
        }
        price
    }
}
