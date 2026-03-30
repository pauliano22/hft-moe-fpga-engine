# =============================================================================
# src/hls/Makefile — Build and test all HLS modules with g++
#
# The HLS modules (#pragma HLS directives) compile cleanly under g++ because:
#   1. Pragmas unknown to g++ are silently ignored
#   2. hls_sim_stubs.hpp provides ap_uint, ap_fixed, hls::stream equivalents
#
# The testbenches reference src/golden_model for the comparison tests.
#
# Targets:
#   make          — build all three testbench binaries
#   make test     — run all testbenches, report PASS/FAIL
#   make clean    — remove build artifacts
#   make vitis    — print instructions for running under Vitis HLS
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I./include -I..

# Paths relative to this Makefile
GM_DIR   := ../golden_model
HLS_DIR  := .

# Testbench binaries
LOB_TB      := lob_tb
ROUTER_TB   := moe_router_tb
EXPERT_TB   := expert_tb

# -------------------------------------------------------
# Default: build all testbenches
# -------------------------------------------------------
.PHONY: all
all: $(LOB_TB) $(ROUTER_TB) $(EXPERT_TB)

# LOB testbench — depends on HLS LOB + golden model (for comparison test)
$(LOB_TB): matching_engine/lob_tb.cpp matching_engine/lob.cpp \
           $(GM_DIR)/itch_parser.cpp $(GM_DIR)/order_book.cpp
	$(CXX) $(CXXFLAGS) \
	    matching_engine/lob_tb.cpp \
	    matching_engine/lob.cpp \
	    $(GM_DIR)/itch_parser.cpp \
	    $(GM_DIR)/order_book.cpp \
	    -o $(LOB_TB)
	@echo "Built: $(LOB_TB)"

# MoE Router testbench
$(ROUTER_TB): moe_router/moe_router_tb.cpp moe_router/moe_router.cpp \
              matching_engine/lob.cpp
	$(CXX) $(CXXFLAGS) \
	    moe_router/moe_router_tb.cpp \
	    moe_router/moe_router.cpp \
	    matching_engine/lob.cpp \
	    -o $(ROUTER_TB)
	@echo "Built: $(ROUTER_TB)"

# Expert kernel testbench
$(EXPERT_TB): experts/expert_tb.cpp experts/expert_kernel.cpp \
              moe_router/moe_router.cpp matching_engine/lob.cpp
	$(CXX) $(CXXFLAGS) \
	    experts/expert_tb.cpp \
	    experts/expert_kernel.cpp \
	    moe_router/moe_router.cpp \
	    matching_engine/lob.cpp \
	    -o $(EXPERT_TB)
	@echo "Built: $(EXPERT_TB)"

# -------------------------------------------------------
# Test: run all testbenches, exit non-zero if any fail
# -------------------------------------------------------
.PHONY: test
test: all
	@echo ""; echo "=== Running LOB testbench ==="
	./$(LOB_TB)
	@echo ""; echo "=== Running MoE Router testbench ==="
	./$(ROUTER_TB)
	@echo ""; echo "=== Running Expert Kernel testbench ==="
	./$(EXPERT_TB)
	@echo ""; echo "=== All HLS C-simulation tests complete ==="

# -------------------------------------------------------
# Clean
# -------------------------------------------------------
.PHONY: clean
clean:
	rm -f $(LOB_TB) $(ROUTER_TB) $(EXPERT_TB)

# -------------------------------------------------------
# Print Vitis HLS instructions (requires manual execution)
# -------------------------------------------------------
.PHONY: vitis
vitis:
	@echo ""
	@echo "=== Vitis HLS Instructions ==="
	@echo ""
	@echo "# RUN MANUALLY — requires Xilinx Vitis HLS 2022.1+"
	@echo ""
	@echo "# Set up Vitis HLS environment first:"
	@echo "  source /opt/Xilinx/Vitis_HLS/2022.1/settings64.sh"
	@echo ""
	@echo "# C-simulation (functional test under Vitis):"
	@echo "  cd matching_engine && vitis_hls -f ../run_csim.tcl lob"
	@echo "  cd moe_router      && vitis_hls -f ../run_csim.tcl moe_router"
	@echo "  cd experts         && vitis_hls -f ../run_csim.tcl expert_kernel"
	@echo ""
	@echo "# Synthesis (generates timing/resource report):"
	@echo "  vitis_hls -f run_synth.tcl"
	@echo ""
	@echo "# Co-simulation (RTL-level verification):"
	@echo "  vitis_hls -f run_cosim.tcl"
	@echo ""
	@echo "# Expected synthesis results (xcvu9p @ 250 MHz):"
	@echo "  LOB:       II=1, Latency ~10 cycles, BRAM=32, DSP=0, FF=~128K"
	@echo "  MoERouter: II=1, Latency ~5  cycles, DSP=32,  FF=~8K"
	@echo "  Expert:    II=1, Latency ~8  cycles, DSP=144, FF=~4K"
