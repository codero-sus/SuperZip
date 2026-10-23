//! g0dszip-cpp — G0D SZIP CLI: Rust container + C++ SZLZRC block engine.
//! This binary is the Rust/C++ merge: Rust owns the .szip layout, safety and
//! CLI; the C++ engine (native/) does the codec math through the C ABI.

use std::env;
use std::fs::File;
use std::io::{BufReader, BufWriter};
use std::process::exit;

use szip_core::codec::SzipError;
use szip_core::container::{compress_with, decompress_with, test_with};
use szip_core::DEFAULT_BLOCK_SIZE;

fn usage() -> ! {
    eprintln!(
        "G0D SZIP — SuperZip compressor (format .szip), Rust+C++ engine\n\
         \n\
         usage:\n\
         \x20 g0dszip-cpp c|compress   [-b blocksize] <input> <output.szip>\n\
         \x20 g0dszip-cpp d|decompress <input.szip> <output>\n\
         \x20 g0dszip-cpp t|test       <input.szip>\n\
         \x20 g0dszip-cpp i|info       <input.szip>\n\
         \n\
         \x20 -b N   raw block size in bytes (default 1048576, max 16777216)"
    );
    exit(2);
}

fn fail(what: &str, path: &str, e: SzipError) -> ! {
    eprintln!("{}: {}: {}", what, path, e);
    exit(1);
}

fn open_in(path: &str) -> BufReader<File> {
    match File::open(path) {
        Ok(f) => BufReader::new(f),
        Err(_) => fail("open", path, SzipError::Io),
    }
}

fn open_out(path: &str) -> BufWriter<File> {
    match File::create(path) {
        Ok(f) => BufWriter::new(f),
        Err(_) => fail("create", path, SzipError::Io),
    }
}

fn enc_cpp(s: &[u8], d: &mut Vec<u8>) -> Result<(), SzipError> {
    match szip_sys::compress_block(s) {
        Ok(v) => {
            d.clear();
            d.extend_from_slice(&v);
            Ok(())
        }
        Err(code) => Err(SzipError::from_code(code)),
    }
}

fn dec_cpp(s: &[u8], d: &mut [u8]) -> Result<(), SzipError> {
    szip_sys::decompress_block(s, d).map_err(SzipError::from_code)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        usage();
    }
    match args[1].as_str() {
        "c" | "compress" => {
            let mut i = 2;
            let mut block_size = DEFAULT_BLOCK_SIZE;
            if i + 1 < args.len() && args[i] == "-b" {
                block_size = match args[i + 1].parse::<u32>() {
                    Ok(v) if v > 0 && v <= (1 << 24) => v,
                    _ => {
                        eprintln!("bad block size: {}", args[i + 1]);
                        exit(2);
                    }
                };
                i += 2;
            }
            if args.len() - i != 2 {
                usage();
            }
            let (input, output) = (&args[i], &args[i + 1]);
            let total = match std::fs::metadata(input) {
                Ok(md) => md.len(),
                Err(_) => fail("open", input, SzipError::Io),
            };
            let mut r = open_in(input);
            let mut w = open_out(output);
            if let Err(e) = compress_with(&mut r, &mut w, total, block_size, &enc_cpp) {
                let _ = std::fs::remove_file(output);
                fail("compress", input, e);
            }
        }
        "d" | "decompress" => {
            if args.len() != 4 {
                usage();
            }
            let (input, output) = (&args[2], &args[3]);
            let mut r = open_in(input);
            let mut w = open_out(output);
            if let Err(e) = decompress_with(&mut r, &mut w, &dec_cpp) {
                let _ = std::fs::remove_file(output);
                fail("decompress", input, e);
            }
        }
        "t" | "test" => {
            if args.len() != 3 {
                usage();
            }
            let mut r = open_in(&args[2]);
            match test_with(&mut r, &dec_cpp) {
                Ok(raw) => println!("{}: ok ({} bytes)", args[2], raw),
                Err(e) => fail("test", &args[2], e),
            }
        }
        "i" | "info" => {
            if args.len() != 3 {
                usage();
            }
            let path = &args[2];
            let mut r = open_in(path);
            match test_with(&mut r, &dec_cpp) {
                Ok(raw) => {
                    let comp = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
                    println!("file       : {}", path);
                    println!("format     : .szip v{}", szip_core::VERSION);
                    println!("codec      : SZLZRC v3 (LZ77 + rep-matches + context-mixing literals)");
                    println!("engine     : szip-sys (C++ native + Rust container)");
                    println!("raw size   : {} bytes", raw);
                    println!("archive    : {} bytes", comp);
                    if raw > 0 {
                        println!(
                            "ratio      : {:.2}%  ({:.3} bits/byte)",
                            100.0 * comp as f64 / raw as f64,
                            8.0 * comp as f64 / raw as f64
                        );
                    }
                    println!("integrity  : CRC32 verified");
                }
                Err(e) => fail("info", path, e),
            }
        }
        "-h" | "--help" | "help" => usage(),
        _ => usage(),
    }
}
