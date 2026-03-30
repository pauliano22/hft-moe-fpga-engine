# =============================================================================
# Golden Model Makefile
#
# Targets:
#   make          — build golden_model binary (release, -O3)
#   make debug    — build with AddressSanitizer + debug symbols
#   make test     — build + run unit tests
#   make bench    — build + run benchmark on data/sample.itch
#   make run      — build + run with --bench --top 10
#   make test-data — build and run the ITCH data generator
#   make clean    — remove all build artifacts
#
# Platform notes:
#   Primarily targets Linux / WSL (required for Vivado/Vitis toolchain).
#   Git Bash on Windows should work for basic builds. For full ASAN support,
#   use WSL (Windows' ASAN support in MinGW is incomplete).
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic

# Release flags: max optimization + native CPU ISA for throughput benchmarking.
# -march=native uses AVX2/AVX-512 if available — numbers reported in the README
# are measured with these flags. Do NOT use -march=native for production deploys.
RELEASE_FLAGS := -O3 -march=native -DNDEBUG

# Debug flags: debug symbols + AddressSanitizer (detects buffer overflows,
# use-after-free, and other memory bugs that Valgrind also finds).
DEBUG_FLAGS   := -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer

# Source files for the main binary
SRCS := itch_parser.cpp order_book.cpp main.cpp
OBJS := $(SRCS:.cpp=.o)

# Source files for the unit test binary
TEST_SRCS := itch_parser.cpp order_book.cpp test.cpp
TEST_OBJS := $(TEST_SRCS:.cpp=.test.o)

# Data generator — lives in ../../tools/
GENERATOR_SRC := ../../tools/generate_test_data.cpp
GENERATOR_BIN := ../../tools/generate_test_data

# Output binaries
BIN      := golden_model
TEST_BIN := golden_model_test

# Data file paths (relative to the Makefile location)
DATA_DIR  := ../../data
SAMPLE    := $(DATA_DIR)/sample.itch

# =============================================================================
# Default target: release build of the golden model binary
# =============================================================================
.PHONY: all
all: $(BIN)

$(BIN): $(SRCS) itch_parser.hpp order_book.hpp
	$(CXX) $(CXXFLAGS) $(RELEASE_FLAGS) $(SRCS) -o $(BIN)
	@echo "Built: $(BIN) (release)"

# =============================================================================
# Debug build with AddressSanitizer
# =============================================================================
.PHONY: debug
debug: $(SRCS) itch_parser.hpp order_book.hpp
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) $(SRCS) -o $(BIN)_debug
	@echo "Built: $(BIN)_debug (ASAN + debug)"

# =============================================================================
# Unit tests
# =============================================================================
.PHONY: test
test: $(TEST_BIN) $(SAMPLE)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) itch_parser.hpp order_book.hpp
	$(CXX) $(CXXFLAGS) $(RELEASE_FLAGS) $(TEST_SRCS) -o $(TEST_BIN)
	@echo "Built: $(TEST_BIN)"

# =============================================================================
# Benchmark: process the sample file and report throughput
# =============================================================================
.PHONY: bench
bench: $(BIN) $(SAMPLE)
	./$(BIN) --file $(SAMPLE) --bench --top 5

# =============================================================================
# Run: build and run with default options
# =============================================================================
.PHONY: run
run: $(BIN) $(SAMPLE)
	./$(BIN) --file $(SAMPLE) --bench --top 10

# =============================================================================
# Generate synthetic test data (1M messages)
# =============================================================================
.PHONY: test-data
test-data: $(GENERATOR_BIN)
	$(GENERATOR_BIN) $(SAMPLE) 1000000
	@echo "Generated: $(SAMPLE)"

$(GENERATOR_BIN): $(GENERATOR_SRC)
	$(CXX) $(CXXFLAGS) -O2 -std=c++17 $(GENERATOR_SRC) -o $(GENERATOR_BIN)
	@echo "Built: $(GENERATOR_BIN)"

# =============================================================================
# Valgrind memory check (Linux only — not available on Windows/MinGW)
# Requires a debug build.
# =============================================================================
.PHONY: valgrind
valgrind: $(TEST_BIN) $(SAMPLE)
	valgrind --leak-check=full --show-leak-kinds=all \
	         --track-origins=yes --error-exitcode=1 \
	         ./$(TEST_BIN)

# =============================================================================
# Clean
# =============================================================================
.PHONY: clean
clean:
	rm -f $(BIN) $(BIN)_debug $(TEST_BIN)
	rm -f *.o
	@echo "Cleaned build artifacts"
