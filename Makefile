# G0D SZIP — build (C++ engine). Rust workspace builds with cargo (see rust/).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wshadow -Wconversion -DNDEBUG
BUILD    := build

NATIVE   := native/codec.cpp native/cli.cpp
TESTS    := native/test.cpp

.PHONY: all clean test

all: $(BUILD)/g0dszip

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/g0dszip: $(NATIVE) native/szip.h native/rc.hpp native/crc32.hpp native/lz77.hpp native/mix.hpp native/tables.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Inative $(NATIVE) -o $@

$(BUILD)/test_szip: $(TESTS) native/codec.cpp native/szip.h native/rc.hpp native/crc32.hpp native/lz77.hpp native/mix.hpp native/tables.hpp | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Inative $(TESTS) native/codec.cpp -o $@

test: $(BUILD)/test_szip $(BUILD)/g0dszip
	$(BUILD)/test_szip
	sh tests/crosscheck.sh
