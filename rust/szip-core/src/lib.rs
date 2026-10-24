//! G0D SZIP — pure-Rust engine for the `.szip` format (SuperZip project).
//!
//! Own encoder and own decoder: LZ77 match finder, adaptive binary range
//! coder, CRC-32 and container layout are all original implementations.
//! No third-party code, no dependencies.

pub mod codec;
pub mod container;
pub mod crc32;
pub mod lz77;
pub mod mix;
pub mod rc;
pub mod tables;

pub use codec::{compress_block, decompress_block, SzipError};
pub use container::{compress_with, decompress_with};

/// Container constants (see docs/SZIP_FORMAT.md).
pub const MAGIC: [u8; 4] = [0x53, 0x5A, 0x50, 0x31]; // "SZP1"
pub const VERSION: u8 = 3;
pub const HEADER_SIZE: usize = 20;
pub const FOOTER_SIZE: usize = 8;
pub const BLOCK_HEADER_SIZE: usize = 10;
pub const BLOCK_STORED: u8 = 0;
pub const BLOCK_SZLZRC: u8 = 1;
pub const DEFAULT_BLOCK_SIZE: u32 = 1 << 20;

/// SZLZRC v3 codec limits.
pub const MIN_MATCH: u32 = 3;
pub const MAX_MATCH: u32 = 2048;
pub const MAX_OFFSET: u32 = 1 << 20;
