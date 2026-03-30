# =============================================================================
# run_synth.tcl — Vitis HLS synthesis script
#
# Synthesizes all three HLS modules and generates timing/resource reports.
# Reports are saved to solution1/syn/report/ in each module directory.
#
# RUN MANUALLY:
#   source /opt/Xilinx/Vitis_HLS/2022.1/settings64.sh
#   cd src/hls && vitis_hls -f run_synth.tcl
#
# Key outputs to capture for the resume/README:
#   - Latency: cycles (look for "Latency" in csynth.rpt)
#   - Initiation Interval: II=1 means one message per clock (look for "Interval")
#   - BRAM, DSP, FF, LUT utilization (look for "Utilization Estimates")
#   - Timing: Clock period achievable (look for "Target" vs "Estimated" in timing)
# =============================================================================

proc synth_module {module top_fn src_files} {
    open_project "${module}_synth"
    set_part {xcvu9p-flga2104-2-i}
    open_solution "solution1" -flow_target vivado

    foreach f $src_files {
        add_files $f
    }
    set_top $top_fn
    create_clock -period 4 -name default   ;# 250 MHz

    # HLS directives are embedded in source via #pragma — no separate tcl directives needed.
    # If you need to add directives here (e.g., for INTERFACE overrides):
    # set_directive_pipeline -II 1 "${top_fn}/MAIN_LOOP"

    # Run synthesis — generates RTL (SystemVerilog/VHDL) and reports
    csynth_design

    # Save reports to a known location
    file copy -force solution1/syn/report/${top_fn}_csynth.rpt \
                     ../../docs/${module}_synthesis.rpt

    close_solution
    close_project
    puts "=== ${module} synthesis complete. Report: docs/${module}_synthesis.rpt ==="
}

# ---- LOB ----
synth_module lob process_messages {
    matching_engine/lob.cpp
}

# ---- MoE Router ----
synth_module moe_router route_message {
    moe_router/moe_router.cpp
    matching_engine/lob.cpp
}

# ---- Expert Kernel ----
synth_module expert_kernel combine_experts {
    experts/expert_kernel.cpp
    moe_router/moe_router.cpp
    matching_engine/lob.cpp
}

puts "=== All synthesis runs complete ==="
puts "=== Open docs/*_synthesis.rpt to see II, latency, and resource usage ==="
exit
