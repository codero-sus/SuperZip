// G0D SZIP — CLI (g0dszip). Own encoder/decoder; no third-party code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "szip.h"

namespace {

const char *err_name(int e) {
  switch (e) {
  case SZIP_OK: return "ok";
  case SZIP_ERR_IO: return "i/o error";
  case SZIP_ERR_FORMAT: return "bad format";
  case SZIP_ERR_CORRUPT: return "corrupt archive";
  case SZIP_ERR_UNSUPPORTED: return "unsupported feature";
  case SZIP_ERR_NOMEM: return "out of memory";
  case SZIP_ERR_TRUNCATED: return "truncated archive";
  case SZIP_ERR_BUFFER: return "buffer too small";
  default: return "unknown error";
  }
}

void usage() {
  std::fprintf(stderr,
               "G0D SZIP — SuperZip compressor (format .szip)\n"
               "\n"
               "usage:\n"
               "  g0dszip c|compress  [-f] [-b blocksize] <input> <output.szip>\n"
               "  g0dszip d|decompress <input.szip> <output>\n"
               "  g0dszip t|test       <input.szip>\n"
               "  g0dszip i|info       <input.szip>\n"
               "  g0dszip g|gui        [port]     lightweight web GUI\n"
               "\n"
               "  -f     fast parse (quicker encode, slightly larger output)\n"
               "  -b N   raw block size in bytes (default 1048576, max 16777216)\n");
}

bool parse_u32(const char *s, uint32_t *v) {
  char *end = nullptr;
  const unsigned long long x = std::strtoull(s, &end, 10);
  if (!end || *end != 0 || x == 0 || x > 0xFFFFFFFFull) return false;
  *v = static_cast<uint32_t>(x);
  return true;
}

int cmd_info(const char *path) {
  uint64_t raw = 0;
  // test_file validates and returns the raw size
  const int e = szip::test_file(path, &raw);
  if (e != SZIP_OK) {
    std::fprintf(stderr, "info: %s: %s\n", path, err_name(e));
    return 1;
  }
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "info: cannot open %s\n", path);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  const long comp = std::ftell(f);
  std::fclose(f);
  std::printf("file       : %s\n", path);
  std::printf("format     : .szip v%d\n", SZIP_VERSION);
  std::printf("codec      : SZLZRC v3 (LZ77 + rep-matches + context-mixing literals)\n");
  std::printf("raw size   : %llu bytes\n", (unsigned long long)raw);
  std::printf("archive    : %ld bytes\n", comp);
  if (raw > 0)
    std::printf("ratio      : %.2f%%  (%.3f bits/byte)\n",
                100.0 * (double)comp / (double)raw,
                8.0 * (double)comp / (double)raw);
  std::printf("integrity  : CRC32 verified\n");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    // GUI is the default entry point: plain `g0dszip` opens it.
    return szip::run_gui(8848);
  }
  const std::string cmd = argv[1];

  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    usage();
    return 0;
  }

  if (cmd == "g" || cmd == "gui") {
    unsigned short port = 8848;
    if (argc >= 3) {
      uint32_t v = 0;
      if (!parse_u32(argv[2], &v) || v > 65535) {
        std::fprintf(stderr, "bad port: %s\n", argv[2]);
        return 2;
      }
      port = static_cast<unsigned short>(v);
    }
    return szip::run_gui(port);
  }

  if (cmd == "c" || cmd == "compress") {
    uint32_t block_size = SZIP_DEFAULT_BLOCK_SIZE;
    int fast = 0;
    int i = 2;
    while (i < argc && argv[i][0] == '-') {
      if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
        if (!parse_u32(argv[i + 1], &block_size)) {
          std::fprintf(stderr, "bad block size: %s\n", argv[i + 1]);
          return 2;
        }
        i += 2;
      } else if (std::strcmp(argv[i], "-f") == 0 ||
                 std::strcmp(argv[i], "--fast") == 0) {
        fast = 1;
        i += 1;
      } else {
        std::fprintf(stderr, "unknown option: %s\n", argv[i]);
        return 2;
      }
    }
    if (argc - i != 2) {
      usage();
      return 2;
    }
    szip::set_parse_fast(fast);
    const int e = szip::compress_file(argv[i], argv[i + 1], block_size);
    if (e != SZIP_OK) {
      std::fprintf(stderr, "compress: %s\n", err_name(e));
      return 1;
    }
    return 0;
  }

  if (cmd == "d" || cmd == "decompress") {
    if (argc != 4) {
      usage();
      return 2;
    }
    const int e = szip::decompress_file(argv[2], argv[3], nullptr, nullptr);
    if (e != SZIP_OK) {
      std::fprintf(stderr, "decompress: %s\n", err_name(e));
      return 1;
    }
    return 0;
  }

  if (cmd == "t" || cmd == "test") {
    if (argc != 3) {
      usage();
      return 2;
    }
    uint64_t raw = 0;
    const int e = szip::test_file(argv[2], &raw);
    if (e != SZIP_OK) {
      std::fprintf(stderr, "test: %s: %s\n", argv[2], err_name(e));
      return 1;
    }
    std::printf("%s: ok (%llu bytes)\n", argv[2], (unsigned long long)raw);
    return 0;
  }

  if (cmd == "i" || cmd == "info") {
    if (argc != 3) {
      usage();
      return 2;
    }
    return cmd_info(argv[2]);
  }

  usage();
  return 2;
}
