// G0D SZIP — sprint codec wall benchmark (in-process, fair vs library codecs).
// Usage: sprintbench <file>...   (times sprint compress+decompress per file)
#include <cstddef>
#include <cstdint>
#include <vector>
namespace szip {
int compress_buffer(const uint8_t *data, size_t len, std::vector<uint8_t> &out,
                    uint32_t block_size);
int decompress_buffer(const uint8_t *data, size_t len, std::vector<uint8_t> &out);
void set_parse_tier(const char *tier);
} // namespace szip
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  szip::set_parse_tier("sprint");
  double enc_total = 0, dec_total = 0;
  long long raw_total = 0, comp_total = 0;
  for (int i = 1; i < argc; ++i) {
    FILE *f = std::fopen(argv[i], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> src((size_t)n);
    if (n && std::fread(src.data(), 1, (size_t)n, f) != (size_t)n) return 2;
    std::fclose(f);
    std::vector<uint8_t> comp, dec;
    // warmup
    szip::compress_buffer(src.data(), src.size(), comp, 65536);
    double be = 1e9, bd = 1e9;
    for (int r = 0; r < 5; ++r) {
      auto t0 = std::chrono::steady_clock::now();
      szip::compress_buffer(src.data(), src.size(), comp, 65536);
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      if (ms < be) be = ms;
      t0 = std::chrono::steady_clock::now();
      dec.clear();
      int rc = szip::decompress_buffer(comp.data(), comp.size(), dec);
      ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
      if (rc != 0 || dec.size() != src.size() ||
          std::memcmp(dec.data(), src.data(), src.size()) != 0) {
        std::printf("ROUNDTRIP FAIL %s\n", argv[i]);
        return 1;
      }
      if (ms < bd) bd = ms;
    }
    enc_total += be;
    dec_total += bd;
    raw_total += n;
    comp_total += (long long)comp.size();
    std::printf("%-36s raw=%8lld comp=%8lld enc=%6.2fms dec=%6.2fms\n", argv[i],
                raw_total, comp_total, enc_total, dec_total);
  }
  std::printf("TOTAL raw=%lld comp=%lld enc=%.2fms dec=%.2fms\n", raw_total,
              comp_total, enc_total, dec_total);
  return 0;
}
