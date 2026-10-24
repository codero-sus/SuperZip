//! Adaptive binary range coder (carryless, 11-bit probabilities).
//! Original implementation, twin of native/rc.hpp.

pub const PROB_INIT: u16 = 1024;
const PROB_BITS: u32 = 11;
const PROB_MOVE_BITS: u32 = 5;
const TOP: u32 = 1 << 24;

pub struct RcEncoder {
    out: Vec<u8>,
    low: u64,
    range: u32,
    cache: u8,
    cache_size: u64,
}

impl RcEncoder {
    pub fn new() -> Self {
        RcEncoder {
            out: Vec::new(),
            low: 0,
            range: 0xFFFF_FFFF,
            cache: 0,
            cache_size: 1,
        }
    }

    pub fn encode_bit(&mut self, p: &mut u16, bit: u32) {
        let bound = (self.range >> PROB_BITS) * (*p as u32);
        if bit == 0 {
            self.range = bound;
            *p = (*p as u32 + ((2048 - *p as u32) >> PROB_MOVE_BITS)) as u16;
        } else {
            self.low += bound as u64;
            self.range -= bound;
            *p = (*p as u32 - ((*p as u32) >> PROB_MOVE_BITS)) as u16;
        }
        while self.range < TOP {
            self.range <<= 8;
            self.shift_low();
        }
    }

    /// Encode with an external probability (no prob update); `p` in
    /// `[1, 2047]` is `P(bit == 0)`.
    pub fn encode_bit_p(&mut self, p: u32, bit: u32) {
        let bound = (self.range >> PROB_BITS) * p;
        if bit == 0 {
            self.range = bound;
        } else {
            self.low += bound as u64;
            self.range -= bound;
        }
        while self.range < TOP {
            self.range <<= 8;
            self.shift_low();
        }
    }

    pub fn flush(&mut self) {
        for _ in 0..5 {
            self.shift_low();
        }
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.out
    }

    fn shift_low(&mut self) {
        let high = (self.low >> 32) as u32;
        if (self.low as u32) < 0xFF00_0000 || high != 0 {
            let mut temp = self.cache;
            loop {
                self.out.push(temp.wrapping_add(high as u8));
                temp = 0xFF;
                self.cache_size -= 1;
                if self.cache_size == 0 {
                    break;
                }
            }
            self.cache = ((self.low as u32) >> 24) as u8;
        }
        self.cache_size += 1;
        self.low = ((self.low as u32) as u64) << 8;
    }
}

impl Default for RcEncoder {
    fn default() -> Self {
        Self::new()
    }
}

pub struct RcDecoder<'a> {
    inp: &'a [u8],
    pos: usize,
    range: u32,
    code: u32,
    ok: bool,
}

impl<'a> RcDecoder<'a> {
    pub fn new(inp: &'a [u8]) -> Self {
        RcDecoder {
            inp,
            pos: 0,
            range: 0xFFFF_FFFF,
            code: 0,
            ok: true,
        }
    }

    /// Reads the 5-byte prologue (first byte must be 0). False on truncation.
    pub fn init(&mut self) -> bool {
        if self.inp.len() < 5 || self.inp[0] != 0 {
            return false;
        }
        self.pos = 1;
        self.code = 0;
        for _ in 0..4 {
            self.code = (self.code << 8) | self.next_byte() as u32;
        }
        self.ok
    }

    pub fn decode_bit(&mut self, p: &mut u16) -> u32 {
        let bound = (self.range >> PROB_BITS) * (*p as u32);
        let bit;
        if self.code < bound {
            self.range = bound;
            *p = (*p as u32 + ((2048 - *p as u32) >> PROB_MOVE_BITS)) as u16;
            bit = 0;
        } else {
            self.code -= bound;
            self.range -= bound;
            *p = (*p as u32 - ((*p as u32) >> PROB_MOVE_BITS)) as u16;
            bit = 1;
        }
        while self.range < TOP {
            self.range <<= 8;
            self.code = (self.code << 8) | self.next_byte() as u32;
        }
        bit
    }

    /// Decode with an external probability (no prob update); `p` in
    /// `[1, 2047]` is `P(bit == 0)`.
    pub fn decode_bit_p(&mut self, p: u32) -> u32 {
        let bound = (self.range >> PROB_BITS) * p;
        let bit;
        if self.code < bound {
            self.range = bound;
            bit = 0;
        } else {
            self.code -= bound;
            self.range -= bound;
            bit = 1;
        }
        while self.range < TOP {
            self.range <<= 8;
            self.code = (self.code << 8) | self.next_byte() as u32;
        }
        bit
    }

    pub fn ok(&self) -> bool {
        self.ok
    }

    pub fn pos(&self) -> usize {
        self.pos
    }

    fn next_byte(&mut self) -> u8 {
        if self.pos >= self.inp.len() {
            self.ok = false;
            return 0;
        }
        let b = self.inp[self.pos];
        self.pos += 1;
        b
    }
}
