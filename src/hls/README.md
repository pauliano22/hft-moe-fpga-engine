# HLS Modules

Three Vitis HLS C++ modules that form the hardware "brain" of the engine:

| Module | Top Function | II Target | Role |
|---|---|---|---|
| `matching_engine/lob.cpp` | `process_messages` | 1 | Maintains bid/ask book, emits best bid/ask |
| `moe_router/moe_router.cpp` | `route_message` | 1 | Extracts features, selects top-2 experts |
| `experts/expert_kernel.cpp` | `combine_experts` | 1 | Runs 2-layer MLP inference, outputs trade signal |

## Building without Vitis HLS (g++ simulation)

```bash
# From repo root (in WSL or Linux):
cd src/hls
make          # build all three testbench binaries
make test     # run all testbenches
```

All three testbenches compile with g++ using `include/hls_sim_stubs.hpp`,
which provides functional equivalents of `ap_uint`, `ap_fixed`, and
`hls::stream`. The `#pragma HLS` directives are silently ignored by g++.

## Running with Vitis HLS

### Prerequisites
- Xilinx Vitis HLS 2022.1 or later
- License: node-locked or floating

```bash
source /opt/Xilinx/Vitis_HLS/2022.1/settings64.sh
cd src/hls
```

### C-Simulation (functional correctness)
```bash
# RUN MANUALLY
vitis_hls -f run_csim.tcl lob
vitis_hls -f run_csim.tcl moe_router
vitis_hls -f run_csim.tcl expert_kernel
```

### Synthesis (timing + resource report)
```bash
# RUN MANUALLY
vitis_hls -f run_synth.tcl
# Reports written to docs/*_synthesis.rpt
```

Look for these values in the synthesis report:
- **Latency (cycles)**: in the "Performance & Resource Estimates" section
- **Initiation Interval**: target II=1 for the MAIN_LOOP / ROUTER_LOOP / EXPERT_LOOP
- **BRAM / DSP / FF / LUT**: in the "Utilization Estimates" section
- **Timing**: "Target" vs "Estimated" clock period — must be ≤ 4.0 ns for 250 MHz

### RTL Co-Simulation
```bash
# RUN MANUALLY — requires synthesis to have run first
vitis_hls -f run_cosim.tcl
```

### Exporting as Vivado IP
```bash
# After synthesis, export as a Vivado IP for top-level integration:
# In Vitis HLS GUI: Solution → Export RTL → Format: Vivado IP (.zip)
# Or via TCL:
# export_design -flow impl -rtl verilog -format ip_catalog
```

## Architecture notes

### Why `ap_fixed<16,6>` for MoE arithmetic?

On Xilinx FPGAs, each DSP48E2 slice performs:
- `float` multiply: consumes 2–3 DSPs, 3-cycle latency
- `ap_fixed<16,6>` multiply: consumes 1 DSP, 1-cycle latency

For the MoE expert (8→16→1 MLP): 128+16 = 144 multiplications.
- Float: 288–432 DSPs, potentially fails timing at 250 MHz
- Fixed-point: 144 DSPs, easily meets 250 MHz timing

### Why `ARRAY_PARTITION complete` on bid/ask arrays?

Without partitioning, a 2048-element array maps to a BRAM with 1 read port
and 1 write port. The FIND_BEST_BID loop needs to read all 2048 elements in
one cycle → BRAM can only supply 1 per cycle → II = 2048.

With `complete` partitioning, every element becomes its own register (FF).
All reads are parallel → II = 1. Cost: 2 × 2048 × 32 = 131,072 FFs ≈ 10%
of xcvu9p's 1.18M FFs — acceptable for a dedicated trading accelerator.

### Price range and tick size

The LOB uses:
- `BASE_PRICE = 1,750,000` ($175.00)
- `TICK_SIZE = 100` ($0.01)
- `MAX_PRICE_LEVELS = 2048`

This covers $175.00 to $195.47. For a different price center, change
`BASE_PRICE` in `lob.hpp`. In production, `BASE_PRICE` would be a runtime
configuration register written by the host CPU via AXI-Lite.
