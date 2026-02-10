# FPGA-Accelerated Sparse MoE Trading Engine

A sub-microsecond, hardware-accelerated trading engine that ingests raw NASDAQ ITCH 5.0 market data and executes trade signals via a Sparse Mixture of Experts (MoE) model — implemented entirely in SystemVerilog and Vitis HLS.

## Performance Targets

| Metric | Target | Proof Method |
|---|---|---|
| Wire-to-Response Latency | < 100ns | Cycle-accurate Verilator trace |
| MoE Inference Latency | < 500ns | Vitis HLS Synthesis Report |
| Peak Throughput | 150M+ msg/s | Stress-test simulation logs |
| Jitter (Tail Latency) | 0ns (Deterministic) | Pipeline analysis in HLS |
| Data Integrity | 100% Bit-Accuracy | Verilator vs. C++ Golden Model |

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                     10GbE MAC Interface                       │
│                    (64-bit AXI-Stream)                        │
└──────────────┬───────────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────────┐
│              ITCH 5.0 Parser ("The Shell")                   │
│              SystemVerilog — Line-rate FSM                    │
│                                                              │
│  ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────┐  │
│  │ Msg Type│→ │  Field   │→ │  Output  │→ │  AXI-Stream │  │
│  │ Detect  │  │ Extract  │  │  Pack    │  │  Master     │  │
│  └─────────┘  └──────────┘  └──────────┘  └─────────────┘  │
└──────────────┬───────────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────────┐
│              MoE Engine ("The Brain")                         │
│              Vitis HLS — Pipelined @ II=1                    │
│                                                              │
│  ┌─────────────┐    ┌────────────────────────────────┐      │
│  │   Router     │    │        Expert Pool             │      │
│  │  (Linear +   │──→ │  ┌─────┐ ┌─────┐ ┌─────┐    │      │
│  │   Top-K)     │    │  │ E0  │ │ E1  │ │ ... │    │      │
│  └─────────────┘    │  │(MLP)│ │(MLP)│ │     │    │      │
│                      │  └──┬──┘ └──┬──┘ └──┬──┘    │      │
│                      │     └───┬───┘       │        │      │
│                      │    Weighted Sum      │        │      │
│                      └────────┬─────────────┘        │      │
│                               ▼                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │           Matching Engine (LOB)                       │   │
│  │     BRAM-Partitioned — O(1) Price-Level Access       │   │
│  └──────────────────────────────────────────────────────┘   │
└──────────────┬───────────────────────────────────────────────┘
               │
               ▼
        ┌──────────────┐
        │ Trade Signal │
        │   Output     │
        └──────────────┘
```

## Technical Stack

### Hardware & Simulation ("Silicon Layer")
- **SystemVerilog** — High-speed binary packet parser, AXI-Stream interfaces
- **Vitis HLS (C++)** — MoE Router, Expert MLPs, Matching Engine
- **Vivado ML** — Synthesis, place & route, timing closure
- **Verilator** — Cycle-accurate C++-compiled simulation

### Verification & Profiling ("Rigor Layer")
- **GDB** — Step-through debugging of C-Simulation
- **Valgrind** — Zero memory leak verification
- **Linux Perf** — Simulation profiling & flame graphs
- **GTKWave** — Waveform visualization

## Repository Structure

```
fpga-moe-trading-engine/
├── src/
│   ├── rtl/                    # SystemVerilog source files
│   │   ├── itch_parser.sv      # ITCH 5.0 line-rate parser
│   │   ├── axi_stream_pkg.sv   # AXI-Stream type definitions
│   │   └── top.sv              # Top-level integration
│   ├── hls/                    # Vitis HLS C++ source files
│   │   ├── moe_router/         # Router network (top-k gating)
│   │   ├── experts/            # Expert MLP kernels
│   │   └── matching_engine/    # Limit Order Book
│   ├── golden_model/           # C++ reference implementation
│   │   ├── itch_parser.cpp     # Software ITCH parser
│   │   ├── moe_model.cpp       # Software MoE inference
│   │   ├── order_book.cpp      # Software LOB
│   │   └── main.cpp            # End-to-end golden model
│   └── tb/                     # Verilator testbenches
│       ├── tb_itch_parser.cpp  # Parser unit test
│       ├── tb_moe_engine.cpp   # MoE integration test
│       └── tb_top.cpp          # Full system test
├── sim/
│   ├── verilator/              # Verilator build artifacts
│   └── scripts/                # Simulation & comparison scripts
├── data/                       # Sample ITCH PCAP files
├── docs/
│   └── images/                 # Waveforms, flame graphs, HLS reports
├── tools/                      # Helper utilities
├── .github/workflows/          # CI pipeline
├── Makefile                    # Build orchestration
└── README.md
```

## Quick Start

### Prerequisites

```bash
# Ubuntu 22.04+ recommended
sudo apt-get update
sudo apt-get install -y \
    verilator \
    gtkwave \
    build-essential \
    cmake \
    valgrind \
    linux-tools-generic \
    libpcap-dev

# Optional: Vitis HLS 2023.2+ (for synthesis — not needed for simulation)
```

### Build & Run

```bash
# 1. Build the C++ Golden Model
make golden_model

# 2. Run golden model against sample ITCH data
make run_golden DATA=data/sample_itch.pcap

# 3. Build Verilator simulation
make verilator_build

# 4. Run cycle-accurate simulation
make verilator_run

# 5. Compare hardware vs. golden model (bit-accuracy check)
make verify

# 6. View waveforms
make waves  # opens GTKWave with trace.fst

# 7. Run full validation suite (Valgrind + Perf + Verify)
make validate
```

## Roadmap

- [x] Repository setup & build system
- [ ] **Day 1-2:** C++ Golden Model — ITCH parser + LOB + MoE inference
- [ ] **Day 3-4:** HLS Matching Engine & MoE kernels with `II=1` pipelining
- [ ] **Day 5:** SystemVerilog ITCH parser (line-rate FSM)
- [ ] **Day 6:** MoE Router + Expert integration in HLS
- [ ] **Day 7:** Full Verilator simulation, performance reports, waveform captures

## Key Design Decisions

### Why Sparse MoE on FPGA?
Dense neural networks waste compute — most parameters are irrelevant for any given market microstate. Sparse MoE activates only 2 of N experts per tick, keeping inference within the sub-microsecond budget while maintaining model capacity across diverse market regimes (trending, mean-reverting, volatile, quiet).

### Why Not GPU?
GPUs add ~10μs of PCIe transfer latency before computation even begins. FPGAs sit directly on the network path — data flows through combinational logic with no memory hierarchy overhead.

### Fixed-Point Quantization
All MoE weights use `ap_fixed<16,8>` (16-bit, 8 integer / 8 fractional). This is validated against a floating-point golden model with a maximum acceptable error of ±1 tick.

## Visual Evidence

Screenshots demonstrating correctness and performance will be added to `docs/images/`:

1. **Waveform** — ITCH packet decoding in GTKWave
2. **HLS Report** — II=1 pipelining proof
3. **Flame Graph** — Simulation efficiency
4. **Valgrind** — Clean memory report
5. **Architecture** — Hardware block diagram

## License

MIT
