# ==============================================================================
# FPGA MoE Trading Engine — Build System
# ==============================================================================

.PHONY: all clean golden_model run_golden verilator_build verilator_run \
        verify waves validate help

# Directories
SRC_DIR     = src
RTL_DIR     = $(SRC_DIR)/rtl
HLS_DIR     = $(SRC_DIR)/hls
GOLDEN_DIR  = $(SRC_DIR)/golden_model
TB_DIR      = $(SRC_DIR)/tb
SIM_DIR     = sim/verilator
BUILD_DIR   = build

# Tools
CXX         = g++
CXXFLAGS    = -std=c++17 -O2 -Wall -Wextra
VERILATOR   = verilator
VL_FLAGS    = --cc --exe --trace-fst --build -Wall \
              -CFLAGS "-std=c++17 -O2" \
              --top-module itch_parser

# ==============================================================================
all: golden_model
	@echo "Build complete. Run 'make help' for available targets."

# ==============================================================================
# Directories
# ==============================================================================
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SIM_DIR):
	mkdir -p $(SIM_DIR)

# ==============================================================================
# C++ Golden Model
# ==============================================================================
golden_model: $(BUILD_DIR)/golden_model

$(BUILD_DIR)/golden_model: $(GOLDEN_DIR)/main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< -lm
	@echo "Golden model built: $@"

run_golden: $(BUILD_DIR)/golden_model
	@echo "=== Running Golden Model ==="
	cd $(BUILD_DIR) && ./golden_model

# ==============================================================================
# Verilator Simulation (requires verilator installed)
# ==============================================================================
verilator_build: | $(SIM_DIR)
	$(VERILATOR) $(VL_FLAGS) \
		-Mdir $(SIM_DIR) \
		$(RTL_DIR)/axi_stream_pkg.sv \
		$(RTL_DIR)/itch_parser.sv \
		$(TB_DIR)/tb_itch_parser.cpp

verilator_run: verilator_build
	@echo "=== Running Verilator Simulation ==="
	cd $(SIM_DIR) && ./Vitch_parser

# ==============================================================================
# Verification
# ==============================================================================
verify: run_golden verilator_run
	@echo "=== Comparing Golden Model vs Hardware ==="
	@echo "Golden trace:  $(BUILD_DIR)/golden_trace.csv"
	@echo "HW waveform:   $(SIM_DIR)/itch_parser.fst"

# ==============================================================================
# Valgrind Memory Check
# ==============================================================================
validate: $(BUILD_DIR)/golden_model
	@echo "=== Valgrind Memory Check ==="
	valgrind --leak-check=full --show-reachable=yes \
		--error-exitcode=1 $(BUILD_DIR)/golden_model

# ==============================================================================
# Waveform Viewer
# ==============================================================================
waves: verilator_run
	gtkwave $(SIM_DIR)/itch_parser.fst &

# ==============================================================================
# Clean
# ==============================================================================
clean:
	rm -rf $(BUILD_DIR) $(SIM_DIR)
	@echo "Cleaned."

# ==============================================================================
# Help
# ==============================================================================
help:
	@echo "FPGA MoE Trading Engine — Build Targets"
	@echo "========================================"
	@echo "  make golden_model    Build C++ golden model"
	@echo "  make run_golden      Run golden model with synthetic data"
	@echo "  make verilator_build Build Verilator simulation"
	@echo "  make verilator_run   Run cycle-accurate simulation"
	@echo "  make verify          Compare golden model vs hardware"
	@echo "  make validate        Run Valgrind memory check"
	@echo "  make waves           Open waveforms in GTKWave"
	@echo "  make clean           Remove build artifacts"
