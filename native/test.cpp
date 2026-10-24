// G0D SZIP — engine tests: roundtrips, edge cases, corruption rejection.
// Built with ASan/UBSan via `make test` flags in the test target below.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "szip.h"

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static std::vector<uint8_t> block_roundtrip(const std::vector<uint8_t> &src,
                                            int *err_out) {
  std::vector<uint8_t> comp(szip_compress_bound(src.size()) + 16);
  size_t clen = 0;
  int e = szip_compress_block(src.data(), src.size(), comp.data(), comp.size(),
                              &clen);
  if (e != SZIP_OK) {
    *err_out = e;
    return {};
  }
  std::vector<uint8_t> dec(src.size());
  e = szip_decompress_block(comp.data(), clen, dec.data(), src.size());
  *err_out = e;
  if (e != SZIP_OK) return {};
  return dec;
}

static void test_rc_and_block_patterns() {
  int e = 0;
  // edge sizes
  for (size_t n = 0; n <= 40; ++n) {
    std::vector<uint8_t> src(n);
    for (size_t i = 0; i < n; ++i) src[i] = (uint8_t)(i * 7 + 3);
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
  // all zeros (RLE-like matches)
  {
    std::vector<uint8_t> src(100000, 0);
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
  // repeated pattern
  {
    std::vector<uint8_t> src;
    const char *pat = "the quick brown fox jumps over the lazy dog. ";
    for (int i = 0; i < 2000; ++i)
      for (const char *p = pat; *p; ++p) src.push_back((uint8_t)*p);
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
  // incompressible random
  {
    std::mt19937 rng(42);
    std::vector<uint8_t> src(50000);
    for (auto &b : src) b = (uint8_t)rng();
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
  // two copies (offset match)
  {
    std::mt19937 rng(7);
    std::vector<uint8_t> half(3000);
    for (auto &b : half) b = (uint8_t)rng();
    std::vector<uint8_t> src = half;
    src.insert(src.end(), half.begin(), half.end());
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
  // long run (overlapping match)
  {
    std::vector<uint8_t> src = {'x', 'y', 'z'};
    src.resize(5000, 'a');
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
  }
}

static void test_random_roundtrips() {
  std::mt19937 rng(20261023);
  for (int iter = 0; iter < 150; ++iter) {
    const size_t n = rng() % 40000;
    std::vector<uint8_t> src(n);
    // mix: some structured, some raw random
    const int style = iter % 3;
    for (size_t i = 0; i < n; ++i) {
      if (style == 0)
        src[i] = (uint8_t)rng();
      else if (style == 1)
        src[i] = (uint8_t)('a' + (rng() % 4));
      else
        src[i] = (uint8_t)((rng() & 3) ? (i * 13) : (uint8_t)rng());
    }
    int e = 0;
    auto dec = block_roundtrip(src, &e);
    CHECK(e == SZIP_OK);
    CHECK(dec == src);
    if (g_failures) {
      std::fprintf(stderr, "  (iter %d n=%zu style=%d)\n", iter, n, style);
      return;
    }
  }
}

static void test_corruption_rejection() {
  std::mt19937 rng(99);
  // Build a valid compressed block of a structured buffer.
  std::vector<uint8_t> src(20000);
  for (size_t i = 0; i < src.size(); ++i)
    src[i] = (uint8_t)((i % 37 == 0) ? 'Z' : (uint8_t)(i & 0xF));
  std::vector<uint8_t> comp(szip_compress_bound(src.size()) + 16);
  size_t clen = 0;
  CHECK(szip_compress_block(src.data(), src.size(), comp.data(), comp.size(),
                            &clen) == SZIP_OK);
  comp.resize(clen);

  int accepted = 0, rejected = 0;
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<uint8_t> bad = comp;
    // flip 1-3 random payload bytes
    const int flips = 1 + (int)(rng() % 3);
    for (int f = 0; f < flips; ++f)
      bad[rng() % bad.size()] ^= (uint8_t)(1u << (rng() % 8));
    std::vector<uint8_t> dec(src.size());
    const int e = szip_decompress_block(bad.data(), bad.size(), dec.data(),
                                        src.size());
    if (e == SZIP_OK) {
      // decoding garbage to valid-looking output is possible in principle;
      // it must at least never crash (ASan enforces memory safety).
      ++accepted;
    } else {
      ++rejected;
    }
  }
  std::printf("corruption: %d rejected, %d accepted-by-decoder (CRC is the "
              "final gate)\n",
              rejected, accepted);
  CHECK(rejected > 200); // vast majority must be rejected by the codec itself

  // truncated payload must always be rejected
  for (size_t cut = 1; cut < comp.size(); cut += 1 + rng() % 5) {
    std::vector<uint8_t> dec(src.size());
    const int e = szip_decompress_block(comp.data(), cut, dec.data(),
                                        src.size());
    CHECK(e != SZIP_OK);
  }
}

static std::string tmp_path(const char *name) {
  return std::string("build/tmp_") + name;
}

static void write_file(const std::string &path, const std::vector<uint8_t> &d) {
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  os.write(reinterpret_cast<const char *>(d.data()), (std::streamsize)d.size());
}

static std::vector<uint8_t> read_file(const std::string &path) {
  std::ifstream is(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(is)),
                              std::istreambuf_iterator<char>());
}

static void file_roundtrip(const std::vector<uint8_t> &data,
                           uint32_t block_size) {
  const std::string in = tmp_path("in.bin");
  const std::string arc = tmp_path("a.szip");
  const std::string out = tmp_path("out.bin");
  write_file(in, data);
  CHECK(szip::compress_file(in, arc, block_size) == SZIP_OK);
  uint64_t raw = 0;
  uint32_t crc = 0;
  CHECK(szip::decompress_file(arc, out, &raw, &crc) == SZIP_OK);
  CHECK(raw == data.size());
  CHECK(read_file(out) == data);
  CHECK(szip::test_file(arc, nullptr) == SZIP_OK);
}

static void test_container() {
  // empty file
  file_roundtrip({}, 1024);
  // 1 byte
  file_roundtrip({42}, 1024);
  // small blocks force multi-block archives
  std::mt19937 rng(5);
  std::vector<uint8_t> data(100000);
  for (size_t i = 0; i < data.size(); ++i)
    data[i] = (uint8_t)((rng() & 7) ? ('A' + (i % 26)) : rng());
  file_roundtrip(data, 4096);
  file_roundtrip(data, 1048576);
  // pure random (forces stored blocks)
  std::vector<uint8_t> rnd(300000);
  for (auto &b : rnd) b = (uint8_t)rng();
  file_roundtrip(rnd, 65536);

  // integrity: corrupt footer CRC -> must fail
  {
    const std::string in = tmp_path("in.bin");
    const std::string arc = tmp_path("a.szip");
    write_file(in, data);
    CHECK(szip::compress_file(in, arc, 4096) == SZIP_OK);
    auto bytes = read_file(arc);
    bytes[bytes.size() - 5] ^= 0xFF; // smash CRC
    write_file(arc, bytes);
    CHECK(szip::decompress_file(arc, tmp_path("out.bin"), nullptr, nullptr) ==
          SZIP_ERR_CORRUPT);
  }
  // integrity: bad magic -> must fail
  {
    auto bytes = read_file(tmp_path("a.szip"));
    bytes[0] = 'X';
    write_file(tmp_path("a.szip"), bytes);
    CHECK(szip::decompress_file(tmp_path("a.szip"), tmp_path("out.bin"),
                                nullptr, nullptr) == SZIP_ERR_FORMAT);
  }
  // integrity: truncation -> must fail
  {
    const std::string in = tmp_path("in.bin");
    const std::string arc = tmp_path("a.szip");
    write_file(in, data);
    CHECK(szip::compress_file(in, arc, 4096) == SZIP_OK);
    auto bytes = read_file(arc);
    bytes.resize(bytes.size() / 2);
    write_file(arc, bytes);
    CHECK(szip::decompress_file(arc, tmp_path("out.bin"), nullptr, nullptr) !=
          SZIP_OK);
  }
}

int main() {
  // known-answer CRC32: "123456789" -> 0xCBF43926
  CHECK(szip_crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) ==
        0xCBF43926u);
  CHECK(szip_crc32(nullptr, 0) == 0u);

  test_rc_and_block_patterns();
  std::printf("patterns ok\n");
  test_random_roundtrips();
  std::printf("random roundtrips ok\n");
  test_corruption_rejection();
  std::printf("corruption rejection ok\n");
  test_container();
  std::printf("container ok\n");

  if (g_failures) {
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
