# =============================================================================
# sim/verilator/Makefile — Verilator simulation build
#
# Targets:
#   make          — compile DUT + harness with Verilator
#   make run      — simulate on data/sample.itch
#   make waves    — open GTKWave on the generated VCD (if installed)
#   make lint     — lint all RTL without building
#   make clean    — remove all generated files
#
# Requires: verilator >= 4.0, g++ (WSL Ubuntu: sudo apt install verilator)
# =============================================================================

VERILATOR  := verilator
CXX        := g++
CXXFLAGS   := -std=c++17 -O2

# Project root (relative to this Makefile)
ROOT       := ../..

# RTL source files
# Only pass top.sv explicitly; itch_parser.sv and order_book.sv are found
# automatically by Verilator via -y (module library search path).
# Listing them explicitly alongside -y causes MODDUP warnings.
RTL_TOP    := $(ROOT)/src/rtl/top.sv
RTL_SRCS   := $(ROOT)/src/rtl/top.sv

# C++ simulation harness
SIM_SRCS   := sim_main.cpp \
               $(ROOT)/src/golden_model/itch_parser.cpp \
               $(ROOT)/src/golden_model/order_book.cpp

# Verilated output directory
OBJ_DIR    := obj_dir

# Top module name (must match module name in top.sv)
TOP        := top

# Verilator flags:
#   --cc           : generate C++ model (not SystemC)
#   --exe          : build an executable
#   --trace        : generate VCD waveform support
#   --Wall         : enable all lint warnings
#   -O3            : optimize generated C++ (faster simulation)
#   --x-assign fast: assert X values as 0 (faster sim, less pessimistic)
#   -y             : module library search path (auto-finds itch_parser.sv, order_book.sv)
VFLAGS     := --cc --exe --trace --Wall -O3 --x-assign fast \
               -y $(ROOT)/src/rtl \
               -CFLAGS "$(CXXFLAGS) -I$(ROOT)/src/golden_model"

# Simulation input data
ITCH_FILE  := $(ROOT)/data/sample.itch

# -------------------------------------------------------
# Default: compile
# -------------------------------------------------------
.PHONY: all
all: $(OBJ_DIR)/V$(TOP)

$(OBJ_DIR)/V$(TOP): $(RTL_SRCS) $(SIM_SRCS)
	@echo "=== Verilating RTL ==="
	$(VERILATOR) $(VFLAGS) --top-module $(TOP) \
	    $(RTL_SRCS) $(SIM_SRCS) \
	    -Mdir $(OBJ_DIR)
	@echo "=== Building simulation binary ==="
	$(MAKE) -C $(OBJ_DIR) -f V$(TOP).mk V$(TOP)
	@echo "=== Build complete: $(OBJ_DIR)/V$(TOP) ==="

# -------------------------------------------------------
# Run simulation
# -------------------------------------------------------
.PHONY: run
run: all $(ITCH_FILE)
	./$(OBJ_DIR)/V$(TOP) --file $(ITCH_FILE) --waves waves.vcd
	@echo ""
	@echo "Latency data written to: sim/verilator/latencies.csv"
	@echo "Waveform written to:     sim/verilator/waves.vcd"

# -------------------------------------------------------
# Open GTKWave (if installed)
# -------------------------------------------------------
.PHONY: waves
waves:
	@if command -v gtkwave >/dev/null 2>&1; then \
	    gtkwave waves.vcd & \
	else \
	    echo "GTKWave not found. Install with: sudo apt install gtkwave"; \
	    echo "Or copy waves.vcd to a Windows machine and open with GTKWave for Windows."; \
	fi

# -------------------------------------------------------
# RTL lint only (fast syntax check)
# -------------------------------------------------------
.PHONY: lint
lint:
	$(VERILATOR) --lint-only -Wall \
	    -y $(ROOT)/src/rtl \
	    $(ROOT)/src/rtl/top.sv
	@echo "Lint complete."

# -------------------------------------------------------
# Clean
# -------------------------------------------------------
.PHONY: clean
clean:
	rm -rf $(OBJ_DIR)
	rm -f waves.vcd latencies.csv
	@echo "Cleaned."
