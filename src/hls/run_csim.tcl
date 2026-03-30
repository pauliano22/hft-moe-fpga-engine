# =============================================================================
# run_csim.tcl — Vitis HLS C-simulation script
#
# RUN MANUALLY:
#   source /opt/Xilinx/Vitis_HLS/2022.1/settings64.sh
#   cd src/hls/matching_engine && vitis_hls -f ../run_csim.tcl lob
#   cd src/hls/moe_router      && vitis_hls -f ../run_csim.tcl moe_router
#   cd src/hls/experts         && vitis_hls -f ../run_csim.tcl expert_kernel
#
# The first argument ($argv) is the module name (lob, moe_router, expert_kernel).
# =============================================================================

set module [lindex $argv 0]

# Create a Vitis HLS project
open_project "${module}_csim"

# Target device — xcvu9p is a high-end UltraScale+ used by many HFT firms.
# For PYNQ/low-cost demo, swap to xc7z020clg400-1 (Zynq-7000).
set_part {xcvu9p-flga2104-2-i}

# Open a solution
open_solution "solution1" -flow_target vivado

# Add source files based on module name
if {$module eq "lob"} {
    add_files matching_engine/lob.cpp
    set_top process_messages
    add_files -tb matching_engine/lob_tb.cpp
    add_files -tb ../golden_model/itch_parser.cpp
    add_files -tb ../golden_model/order_book.cpp
} elseif {$module eq "moe_router"} {
    add_files moe_router/moe_router.cpp
    add_files matching_engine/lob.cpp
    set_top route_message
    add_files -tb moe_router/moe_router_tb.cpp
} elseif {$module eq "expert_kernel"} {
    add_files experts/expert_kernel.cpp
    add_files moe_router/moe_router.cpp
    add_files matching_engine/lob.cpp
    set_top combine_experts
    add_files -tb experts/expert_tb.cpp
}

# Clock: 250 MHz = 4 ns period. This is the target for the full pipeline.
create_clock -period 4 -name default

# Run C-simulation
# This compiles and runs the testbench in software, verifying functional correctness.
# Failures here are pure logic bugs (no hardware involved yet).
csim_design -clean

# Save and exit
close_solution
close_project
exit
