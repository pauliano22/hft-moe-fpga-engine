# =============================================================================
# run_cosim.tcl — Vitis HLS RTL co-simulation script
#
# Co-simulation (cosim) compiles the synthesized RTL (SystemVerilog), wraps
# it with the original C++ testbench, and simulates the actual hardware
# behavior. This is the gold standard for verifying that synthesis preserved
# functional correctness — a C-sim pass + cosim fail means a synthesis bug.
#
# RUN MANUALLY (after run_synth.tcl):
#   source /opt/Xilinx/Vitis_HLS/2022.1/settings64.sh
#   cd src/hls && vitis_hls -f run_cosim.tcl
#
# Key output: co-simulation latency in clock cycles.
# Capture the "Latency" column from the cosim report — this is the number
# you put in the README as "MoE inference: X cycles at 250MHz = Y ns".
# =============================================================================

proc cosim_module {module top_fn src_files tb_files} {
    open_project "${module}_synth"    ;# reuse the synthesis project
    open_solution "solution1"

    foreach f $src_files { add_files $f }
    set_top $top_fn
    foreach f $tb_files  { add_files -tb $f }
    create_clock -period 4 -name default

    # Run cosim using xsim (Vivado's built-in simulator, no license needed).
    # Alternatives: modelsim, vcs, riviera (require additional licenses).
    cosim_design -tool xsim -trace_level all -wave_debug

    # The -wave_debug flag generates a VCD waveform at:
    # solution1/sim/wrapc_pc/xsim_proj/*.wdb
    # Open with: xsim --gui <wdb_file>
    # Or export to VCD: File > Export > Export Waveform > VCD

    close_solution
    close_project
    puts "=== ${module} co-simulation complete ==="
}

cosim_module lob process_messages \
    {matching_engine/lob.cpp} \
    {matching_engine/lob_tb.cpp ../golden_model/itch_parser.cpp ../golden_model/order_book.cpp}

cosim_module moe_router route_message \
    {moe_router/moe_router.cpp matching_engine/lob.cpp} \
    {moe_router/moe_router_tb.cpp}

cosim_module expert_kernel combine_experts \
    {experts/expert_kernel.cpp moe_router/moe_router.cpp matching_engine/lob.cpp} \
    {experts/expert_tb.cpp}

exit
