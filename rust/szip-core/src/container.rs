//! `.szip` container (file layout) — Rust implementation, twin of the C++
//! container layer. The block codec is pluggable so the C++ engine can be
//! used interchangeably (same bytes in, same bytes out).

use std::io::{Read, Write};

use crate::codec::SzipError;
use crate::crc32::crc32_update;
use crate::{
    BLOCK_HEADER_SIZE, BLOCK_STORED, BLOCK_SZLZRC, FOOTER_SIZE, HEADER_SIZE, MAGIC, VERSION,
};

/// Block compressor signature: appends the SZLZRC payload to `out`.
pub type CompressFn<'a> = &'a dyn Fn(&[u8], &mut Vec<u8>) -> Result<(), SzipError>;
/// Block decompressor signature: decodes payload into `dst` (len = raw size).
pub type DecompressFn<'a> = &'a dyn Fn(&[u8], &mut [u8]) -> Result<(), SzipError>;

fn put_u16(out: &mut Vec<u8>, v: u16) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn put_u32(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn put_u64(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn read_exact_n<R: Read>(r: &mut R, n: usize) -> Result<Vec<u8>, SzipError> {
    let mut buf = vec![0u8; n];
    r.read_exact(&mut buf).map_err(|_| SzipError::Truncated)?;
    Ok(buf)
}

fn get_u16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

fn get_u32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

fn get_u64(b: &[u8]) -> u64 {
    u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]])
}

/// Compress `total_raw` bytes from `reader` into a `.szip` stream on `writer`.
pub fn compress_with<R: Read, W: Write>(
    reader: &mut R,
    writer: &mut W,
    total_raw: u64,
    block_size: u32,
    enc: CompressFn<'_>,
) -> Result<(), SzipError> {
    if block_size == 0 || block_size > (1 << 24) {
        return Err(SzipError::Format);
    }

    // header
    let mut hdr = Vec::with_capacity(HEADER_SIZE);
    hdr.extend_from_slice(&MAGIC);
    hdr.push(VERSION);
    hdr.push(0); // flags
    put_u16(&mut hdr, 0); // reserved
    put_u32(&mut hdr, block_size);
    put_u64(&mut hdr, total_raw);
    writer.write_all(&hdr).map_err(|_| SzipError::Io)?;

    let mut payload: Vec<u8> = Vec::new();
    let mut crc = 0u32;
    let mut remaining = total_raw;

    while remaining > 0 {
        let want = std::cmp::min(block_size as u64, remaining) as usize;
        let block = read_exact_n(reader, want)?;
        crc = crc32_update(crc, &block);

        enc(&block, &mut payload)?;
        let store = payload.len() >= want;
        let comp_size = if store { want } else { payload.len() };

        let mut bh = [0u8; BLOCK_HEADER_SIZE];
        bh[0] = if store { BLOCK_STORED } else { BLOCK_SZLZRC };
        bh[1] = 0;
        bh[2..6].copy_from_slice(&(want as u32).to_le_bytes());
        bh[6..10].copy_from_slice(&(comp_size as u32).to_le_bytes());
        writer.write_all(&bh).map_err(|_| SzipError::Io)?;
        if store {
            writer.write_all(&block).map_err(|_| SzipError::Io)?;
        } else {
            writer.write_all(&payload[..comp_size]).map_err(|_| SzipError::Io)?;
        }

        remaining -= want as u64;
    }

    let mut ft = Vec::with_capacity(FOOTER_SIZE);
    put_u32(&mut ft, crc);
    ft.extend_from_slice(&MAGIC);
    writer.write_all(&ft).map_err(|_| SzipError::Io)?;
    Ok(())
}

/// Decompress a `.szip` stream from `reader` into `writer`.
/// Returns the raw size.
pub fn decompress_with<R: Read, W: Write>(
    reader: &mut R,
    writer: &mut W,
    dec: DecompressFn<'_>,
) -> Result<u64, SzipError> {
    let hdr = read_exact_n(reader, HEADER_SIZE)?;
    if hdr[0..4] != MAGIC {
        return Err(SzipError::Format);
    }
    if hdr[4] != VERSION {
        return Err(SzipError::Unsupported);
    }
    let block_size = get_u32(&hdr[8..12]);
    let total_raw = get_u64(&hdr[12..20]);
    if block_size == 0 || block_size > (1 << 24) {
        return Err(SzipError::Format);
    }

    let mut crc = 0u32;
    let mut produced = 0u64;

    while produced < total_raw {
        let bh = read_exact_n(reader, BLOCK_HEADER_SIZE)?;
        let typ = bh[0];
        let raw_size = get_u32(&bh[2..6]) as usize;
        let comp_size = get_u32(&bh[6..10]) as usize;

        if raw_size == 0 || raw_size > block_size as usize {
            return Err(SzipError::Corrupt);
        }
        if raw_size as u64 > total_raw - produced {
            return Err(SzipError::Corrupt);
        }
        if typ == BLOCK_STORED {
            if comp_size != raw_size {
                return Err(SzipError::Corrupt);
            }
        } else if typ == BLOCK_SZLZRC {
            if comp_size < 5 || comp_size > crate::codec::compress_bound(raw_size) {
                return Err(SzipError::Corrupt);
            }
        } else {
            return Err(SzipError::Unsupported);
        }

        let payload = read_exact_n(reader, comp_size)?;
        let mut raw = vec![0u8; raw_size];
        if typ == BLOCK_STORED {
            raw.copy_from_slice(&payload);
        } else {
            dec(&payload, &mut raw)?;
        }

        crc = crc32_update(crc, &raw);
        writer.write_all(&raw).map_err(|_| SzipError::Io)?;
        produced += raw_size as u64;
    }

    let ft = read_exact_n(reader, FOOTER_SIZE)?;
    let stored_crc = get_u32(&ft[0..4]);
    if ft[4..8] != MAGIC {
        return Err(SzipError::Format);
    }
    if stored_crc != crc {
        return Err(SzipError::Corrupt);
    }
    Ok(produced)
}

/// Validate a `.szip` stream without writing output. Returns the raw size.
pub fn test_with<R: Read>(reader: &mut R, dec: DecompressFn<'_>) -> Result<u64, SzipError> {
    let mut sink = std::io::sink();
    decompress_with(reader, &mut sink, dec)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::{compress_block, decompress_block};

    fn roundtrip(data: &[u8], block_size: u32) {
        let mut arc = Vec::new();
        let mut reader = std::io::Cursor::new(data);
        compress_with(
            &mut reader,
            &mut arc,
            data.len() as u64,
            block_size,
            &|s, d| compress_block(s, d),
        )
        .unwrap();

        let mut out = Vec::new();
        let mut r = std::io::Cursor::new(&arc);
        let n = decompress_with(&mut r, &mut out, &|s, d| decompress_block(s, d)).unwrap();
        assert_eq!(n, data.len() as u64);
        assert_eq!(out, data);
    }

    #[test]
    fn container_roundtrip() {
        roundtrip(b"", 1024);
        roundtrip(b"A", 1024);
        let mut v = Vec::new();
        for i in 0..100000u32 {
            v.push(if i % 7 == 0 { b'Q' } else { (i % 26 + b'a') as u8 });
        }
        roundtrip(&v, 4096);
        roundtrip(&v, 1 << 20);
    }

    #[test]
    fn rejects_corruption() {
        let mut v = Vec::new();
        for i in 0..20000u32 {
            v.push(if i % 37 == 0 { b'Z' } else { (i & 0xF) as u8 });
        }
        let mut arc = Vec::new();
        let mut reader = std::io::Cursor::new(&v);
        compress_with(
            &mut reader,
            &mut arc,
            v.len() as u64,
            4096,
            &|s, d| compress_block(s, d),
        )
        .unwrap();

        // smash footer CRC
        let mut bad = arc.clone();
        let n = bad.len();
        bad[n - 5] ^= 0xFF;
        let mut out = Vec::new();
        let mut r = std::io::Cursor::new(&bad);
        assert_eq!(
            decompress_with(&mut r, &mut out, &|s, d| decompress_block(s, d)),
            Err(SzipError::Corrupt)
        );

        // bad magic
        let mut bad = arc.clone();
        bad[0] = b'X';
        let mut r = std::io::Cursor::new(&bad);
        assert_eq!(
            decompress_with(&mut r, &mut out, &|s, d| decompress_block(s, d)),
            Err(SzipError::Format)
        );

        // truncation
        let mut bad = arc.clone();
        bad.truncate(arc.len() / 2);
        let mut r = std::io::Cursor::new(&bad);
        assert!(decompress_with(&mut r, &mut out, &|s, d| decompress_block(s, d)).is_err());
    }
}
