/* G0D SZIP — C ABI for the SZLZRC block engine (SuperZip project).
 * Own encoder/decoder, no third-party compression code.
 */
#ifndef SZIP_H
#define SZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (shared with the Rust engines). */
#define SZIP_OK 0
#define SZIP_ERR_IO (-1)
#define SZIP_ERR_FORMAT (-2)
#define SZIP_ERR_CORRUPT (-3)
#define SZIP_ERR_UNSUPPORTED (-4)
#define SZIP_ERR_NOMEM (-5)
#define SZIP_ERR_TRUNCATED (-6)
#define SZIP_ERR_BUFFER (-7)

/* Container constants. */
#define SZIP_VERSION 3
#define SZIP_MAGIC0 0x53 /* 'S' */
#define SZIP_MAGIC1 0x5A /* 'Z' */
#define SZIP_MAGIC2 0x50 /* 'P' */
#define SZIP_MAGIC3 0x31 /* '1' */
#define SZIP_HEADER_SIZE 20
#define SZIP_FOOTER_SIZE 8
#define SZIP_BLOCK_HEADER_SIZE 10
#define SZIP_BLOCK_STORED 0
#define SZIP_BLOCK_SZLZRC 1
#define SZIP_DEFAULT_BLOCK_SIZE (1u << 20)

/* Codec limits (SZLZRC v1). */
#define SZIP_MIN_MATCH 3
#define SZIP_MAX_MATCH 2048
#define SZIP_MAX_OFFSET (1u << 20)

/* Upper bound for the compressed size of `raw_len` bytes. */
size_t szip_compress_bound(size_t raw_len);

/* Compress one block. dst_len receives the payload size written to dst.
 * Returns SZIP_OK, SZIP_ERR_BUFFER if dst_cap is too small. */
int szip_compress_block(const uint8_t *src, size_t src_len, uint8_t *dst,
                        size_t dst_cap, size_t *dst_len);

/* Decompress one SZLZRC block. `raw_len` is the expected raw size from the
 * block header. Returns SZIP_OK, or a negative error for corrupt input. */
int szip_decompress_block(const uint8_t *src, size_t src_len, uint8_t *dst,
                          size_t raw_len);

/* IEEE CRC-32 (poly 0xEDB88320). */
uint32_t szip_crc32(const uint8_t *data, size_t len);
uint32_t szip_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
} /* extern "C" */

#include <string>
#include <vector>

namespace szip {

/* ---- container (.szip file layout) — C++ reference implementation ---- */

struct Header {
  uint32_t block_size = SZIP_DEFAULT_BLOCK_SIZE;
  uint64_t total_raw = 0;
};

int compress_file(const std::string &in_path, const std::string &out_path,
                  uint32_t block_size = SZIP_DEFAULT_BLOCK_SIZE);
int decompress_file(const std::string &in_path, const std::string &out_path,
                    uint64_t *raw_out = nullptr, uint32_t *crc_out = nullptr);
int test_file(const std::string &in_path, uint64_t *raw_out = nullptr);

} /* namespace szip */
#endif /* __cplusplus */

#endif /* SZIP_H */
