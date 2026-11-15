//! G0D SZIP — FFI bindings to the C++ block engine (`native/codec.cpp`).
//!
//! This crate is the merge point: Rust owns the `.szip` container and the
//! CLI safety model; the C++ engine provides the SZLZRC block codec through
//! a C ABI. Both engines speak the exact same format.

use std::os::raw::{c_int, c_uint};

pub const SZIP_OK: c_int = 0;

/// Upper bound for the compressed size of `raw_len` bytes.
pub fn compress_bound(raw_len: usize) -> usize {
    unsafe { szip_compress_bound(raw_len) }
}

/// Compress one block with the C++ engine. Returns the payload.
pub fn compress_block(src: &[u8]) -> Result<Vec<u8>, i32> {
    let mut dst = vec![0u8; compress_bound(src.len())];
    let mut dst_len: usize = 0;
    let rc = unsafe {
        szip_compress_block(
            src.as_ptr(),
            src.len(),
            dst.as_mut_ptr(),
            dst.len(),
            &mut dst_len,
        )
    };
    if rc != SZIP_OK {
        return Err(rc);
    }
    dst.truncate(dst_len);
    Ok(dst)
}

/// Decompress one SZLZRC block with the C++ engine.
pub fn decompress_block(src: &[u8], dst: &mut [u8]) -> Result<(), i32> {
    let rc = unsafe { szip_decompress_block(src.as_ptr(), src.len(), dst.as_mut_ptr(), dst.len()) };
    if rc != SZIP_OK {
        return Err(rc);
    }
    Ok(())
}

/// IEEE CRC-32 (C++ engine parity).
pub fn crc32(data: &[u8]) -> u32 {
    unsafe { szip_crc32(data.as_ptr(), data.len()) }
}

extern "C" {
    fn szip_compress_bound(raw_len: usize) -> usize;
    fn szip_compress_block(
        src: *const u8,
        src_len: usize,
        dst: *mut u8,
        dst_cap: usize,
        dst_len: *mut usize,
    ) -> c_int;
    fn szip_decompress_block(
        src: *const u8,
        src_len: usize,
        dst: *mut u8,
        raw_len: usize,
    ) -> c_int;
    fn szip_crc32(data: *const u8, len: usize) -> c_uint;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpp_engine_roundtrip() {
        let mut src = Vec::new();
        for i in 0..50000u32 {
            src.push(if i % 13 == 0 { b'K' } else { (i % 251) as u8 });
        }
        let comp = compress_block(&src).unwrap();
        let mut dec = vec![0u8; src.len()];
        decompress_block(&comp, &mut dec).unwrap();
        assert_eq!(src, dec);
    }

    #[test]
    fn crc_matches_known() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }
}
