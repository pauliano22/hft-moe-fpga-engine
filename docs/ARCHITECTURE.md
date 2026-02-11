# Architecture Deep Dive

This document explains every component of the FPGA MoE Trading Engine — what it does, why it exists, and how data flows through the system.

## Table of Contents

1. [The Big Picture](#the-big-picture)
2. [Why FPGA? Why Not GPU or CPU?](#why-fpga-why-not-gpu-or-cpu)
3. [Data Flow: End to End](#data-flow-end-to-end)
4. [Stage 1: ITCH 5.0 Parser (SystemVerilog)](#stage-1-itch-50-parser)
5. [Stage 2: MoE Router (HLS)](#stage-2-moe-router)
6. [Stage 3: Expert MLPs (HLS)](#stage-3-expert-mlps)
7. [Stage 4: Matching Engine / LOB (HLS)](#stage-4-matching-engine)
8. [The Golden Model (C++)](#the-golden-model)
9. [Verification Strategy](#verification-strategy)
10. [Fixed-Point Arithmetic](#fixed-point-arithmetic)
11. [AXI-Stream Protocol](#axi-stream-protocol)
12. [Key Design Decisions](#key-design-decisions)
13. [Performance Analysis](#performance-analysis)

---

## The Big Picture

This project implements a **complete trading pipeline in hardware**:

```
Market Data (ITCH) → Parse → AI Decision (MoE) → Execute (LOB) → Trade Signal
        │                │              │                │              │
    10GbE wire     SystemVerilog    Vitis HLS       Vitis HLS      Output
    (raw bytes)    (RTL FSM)       (C++ → gates)   (C++ → gates)  (action)
```

**In plain English:** Raw stock market data arrives over a 10 Gigabit Ethernet wire as a stream of bytes. Our FPGA chip:
1. **Parses** the bytes to understand "someone wants to buy 100 shares of AAPL at $150"
2. **Runs AI** (a neural network called Mixture of Experts) to decide "should we trade?"
3. **Checks the order book** to see if we can match against existing orders
4. **Outputs a trade signal** — all in under 100 nanoseconds

For context, 100 nanoseconds is the time it takes light to travel 30 meters. A CPU-based system would take 1-10 *micro*seconds — 10x to 100x slower.

---

## Why FPGA? Why Not GPU or CPU?

### The Latency Ladder

| Platform | Typical Latency | Why |
|----------|----------------|-----|
| **CPU** | 1-10 μs | OS scheduling, cache misses, branch mispredictions |
| **GPU** | 10-50 μs | PCIe transfer latency (~10μs) before compute even starts |
| **FPGA** | 10-100 ns | Data flows through logic gates — no instruction fetch, no memory hierarchy |
| **ASIC** | 1-10 ns | Custom silicon — but costs $10M+ to manufacture |

### Why FPGAs Win for Trading

1. **Deterministic latency**: Every clock cycle does the same work. No jitter from garbage collection, context switches, or cache behavior. In trading, *consistency* matters as much as raw speed.

2. **Wire-to-wire processing**: The FPGA sits directly on the network path. Data bytes enter one side of the chip and trade decisions exit the other side — no PCIe bus, no DMA, no kernel drivers in the way.

3. **Spatial parallelism**: Instead of executing instructions one at a time (like a CPU), an FPGA runs *everything simultaneously*. The parser, AI model, and order book all operate in parallel as physical circuits.

4. **Reprogrammability**: Unlike an ASIC, we can update the FPGA's logic without manufacturing new hardware. Change the AI model? Just recompile and reprogram.

---

## Data Flow: End to End

```
┌─────────────────────────────────────────────────────────────────────┐
│                        FPGA Chip (Simplified)                       │
│                                                                     │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────────────┐│
│  │  10GbE   │──→│  ITCH    │──→│   MoE    │──→│    Matching      ││
│  │  MAC     │   │  Parser  │   │  Engine  │   │    Engine (LOB)  ││
│  │          │   │          │   │          │   │                  ││
│  │ Raw bytes│   │ Structured│   │ Trade    │   │ Trade Signal     ││
│  │ 64b/clk │   │ order    │   │ decision │   │ (Buy/Sell/Hold)  ││
│  └──────────┘   └──────────┘   └──────────┘   └──────────────────┘│
│                                                                     │
│  Interface: ────AXI-Stream────→────AXI-Stream──→────AXI-Stream────→│
└─────────────────────────────────────────────────────────────────────┘
```

### Clock-by-Clock: What Happens When a Market Data Packet Arrives

1. **Clock 0**: 10GbE MAC delivers the first 8 bytes on `s_axis_tdata[63:0]`. The ITCH parser sees byte 0 = `0x41` ('A' = Add Order) and enters the parsing FSM.

2. **Clocks 1-4**: The parser accumulates timestamp, order reference, side, shares, stock symbol, and price across 5 beats of 8 bytes each (36 bytes total for an Add Order).

3. **Clock 5**: The parser asserts `order_out.valid = 1` for exactly one cycle, presenting all parsed fields to the next stage.

4. **Clock 6**: The MoE router reads the parsed order, extracts features, and computes gating scores for all 8 experts in a single cycle (fully unrolled/pipelined).

5. **Clock 7**: The top-2 expert MLPs compute their outputs in parallel.

6. **Clock 8**: The weighted expert outputs are combined into a trade decision (Buy/Sell/Hold + confidence score).

7. **Clock 9**: The matching engine checks the Limit Order Book and produces a trade signal.

**Total: ~10 clock cycles at 250MHz = 40 nanoseconds.** (Actual latency depends on synthesis results.)

---

## Stage 1: ITCH 5.0 Parser

**File**: `src/rtl/itch_parser.sv`

### What is ITCH?

ITCH 5.0 is NASDAQ's native market data feed protocol. It's a binary protocol — unlike JSON or XML, every field is a fixed number of bytes at a known offset. This makes it ideal for hardware parsing: no string matching, no variable-length fields, just byte extraction.

### How the Parser Works

The parser is a **Finite State Machine (FSM)** that processes one 64-bit AXI-Stream beat (8 bytes) per clock cycle:

```
            ┌─────────┐  msg_type == 'A'  ┌─────────┐
  ─────────→│  S_IDLE  │─────────────────→│ S_HEADER│
            │ (byte 0) │                   │(bytes 8+)│
            └─────┬────┘                   └────┬────┘
                  │ msg_type != 'A'              │
                  ▼                              ▼
            ┌─────────┐                   ┌──────────┐
            │ S_SKIP  │                   │S_ORDER_  │
            │(to TLAST)│                   │  REF     │
            └─────────┘                   └────┬─────┘
                                               │
                                               ▼
                                        ┌──────────┐
                                        │ S_STOCK  │
                                        └────┬─────┘
                                               │
                                               ▼
                                        ┌──────────┐
                                        │ S_PRICE  │──→ order_out.valid = 1
                                        └──────────┘
```

**Key insight**: The parser never buffers an entire message. It extracts bytes *as they arrive* using the `get_byte()` function, which indexes into the current 64-bit beat. This means parsing takes exactly 5 clock cycles (5 beats × 8 bytes = 40 bytes ≥ 36 byte message).

### ITCH Add Order Message Layout

```
Byte:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
     ┌──┬─────┬─────┬──────────────┬────────────────────
     │'A│Locte│Track│  Timestamp   │   Order Reference
     │  │     │ Num │  (6 bytes)   │     (8 bytes)
     └──┴─────┴─────┴──────────────┴────────────────────

Byte: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35
     ─────────────┬──┬───────────┬───────────────────────┬──────────┐
      Ref (cont)  │BS│  Shares   │    Stock Symbol        │  Price   │
                  │  │ (4 bytes) │    (8 bytes ASCII)     │(4 bytes) │
     ─────────────┴──┴───────────┴───────────────────────┴──────────┘
```

---

## Stage 2: MoE Router

**Files**: `src/hls/moe_router/moe_router.h`, `moe_router.cpp`

### What is Mixture of Experts (MoE)?

MoE is a neural network architecture where instead of one big network, you have many small "expert" networks and a "router" that picks which experts to use for each input. Think of it like a hospital:

- **Dense network** = one doctor who does everything (slow, overworked)
- **MoE network** = reception desk (router) + specialist doctors (experts)

The router looks at your symptoms (input features) and sends you to the 2 most relevant specialists. This is much faster because:
- Only 2 of N experts run (not all N) — **sparse activation**
- Each expert is small and specialized
- Different market conditions route to different experts

### Router Algorithm

```
Input: Feature vector [f0, f1, ..., f7]

Step 1: Score each expert
  score[e] = bias[e] + Σ(weight[e][f] × feature[f])  for each expert e

Step 2: Top-K selection (K=2)
  Find the 2 experts with the highest scores

Step 3: Softmax approximation
  Convert scores to gating weights that sum to ~1.0
  Uses piecewise-linear sigmoid (hardware-friendly, no exp() needed)

Step 4: Dispatch
  Send the feature vector + gating weight to each selected expert
```

### HLS Pragmas Explained

```cpp
#pragma HLS PIPELINE II=1      // Process one input per clock cycle
#pragma HLS UNROLL             // Convert loop into parallel hardware
#pragma HLS ARRAY_PARTITION    // Split array into separate BRAM banks
#pragma HLS INTERFACE axis     // Use AXI-Stream protocol for I/O
#pragma HLS INTERFACE bram     // Store this array in Block RAM
```

---

## Stage 3: Expert MLPs

**Files**: `src/hls/experts/expert_mlp.h`, `expert_mlp.cpp`

Each expert is a 2-layer Multi-Layer Perceptron (MLP):

```
Input (8 features)
        │
        ▼
┌───────────────┐
│   Layer 1     │  8 inputs × 16 hidden neurons
│   (Linear)    │  + bias + ReLU activation
└───────┬───────┘
        │ 16 values
        ▼
┌───────────────┐
│   Layer 2     │  16 hidden → 1 scalar output
│   (Linear)    │  + bias
└───────┬───────┘
        │ 1 value
        ▼
  Expert Output (scalar)
```

**ReLU activation**: `relu(x) = max(0, x)`. In hardware, this is literally checking the sign bit — if negative, output zero. One of the simplest possible activation functions.

**Why MLP?** MLPs are the simplest neural network architecture that can learn nonlinear patterns. In HLS, each multiply-accumulate becomes a DSP slice on the FPGA. With `#pragma HLS UNROLL`, all 16 hidden neurons compute simultaneously.

---

## Stage 4: Matching Engine

**Files**: `src/hls/matching_engine/matching_engine.h`, `matching_engine.cpp`

### What is a Limit Order Book (LOB)?

A LOB tracks all outstanding buy and sell orders for a stock, organized by price:

```
     SELL (Ask) Side          BUY (Bid) Side
     ──────────────          ──────────────
     $150.05  ×200           $150.00  ×100   ← Best Bid
     $150.10  ×500           $149.95  ×300
     $150.15  ×150           $149.90  ×200
         ↑                       ↑
     Best Ask               Prices decrease
     (lowest sell)          downward
```

**The spread** is the gap between best bid and best ask ($150.05 - $150.00 = $0.05 in this example). This is where market makers profit.

### How Matching Works

When a new order arrives:
1. **Buy order at $150.10**: Price ≥ best ask ($150.05), so it **crosses** — we match against the ask, fill 200 shares at $150.05, and the remaining quantity goes on the bid book.
2. **Sell order at $149.80**: Price ≤ best bid ($150.00), so it **crosses** — we match against the bid.
3. **Buy order at $149.90**: Price < best ask, so **no match** — add to bid book.

### BRAM-Partitioned Design

Instead of searching through a sorted list (slow), we use the price itself as a BRAM address:

```
BRAM Address = Price in ticks
BRAM[1000] = quantity at $10.00
BRAM[1001] = quantity at $10.01
BRAM[1002] = quantity at $10.02
...
```

This gives O(1) price-level access — no searching needed. The FPGA's Block RAM is perfect for this because it provides single-cycle read/write access.

---

## The Golden Model

**File**: `src/golden_model/main.cpp`

The golden model is a **pure C++ implementation** of the entire pipeline. It exists for one reason: to provide ground truth for verification.

### Components (all in one file for simplicity)

1. **FixedPoint class**: Emulates `ap_fixed<16,8>` arithmetic using `int16_t`. This ensures the C++ model uses the same fixed-point math as the HLS hardware.

2. **ITCHParser class**: Parses raw ITCH byte buffers, matching the RTL parser byte-for-byte.

3. **FeatureExtractor class**: Converts raw order data into the 8-feature vector that feeds the MoE.

4. **MoEModel class**: Full MoE inference — router scoring, top-K selection, softmax, expert MLPs, weighted combination, and final decision.

5. **OrderBook class**: Software LOB using `std::map` (red-black tree) — different data structure than the hardware, but produces identical matching results.

### Why "Golden"?

In hardware verification, the "golden model" is the trusted reference. You run the same inputs through both the golden model and the hardware, then compare outputs field by field. If they match, the hardware is correct.

---

## Verification Strategy

```
                    ┌──────────────┐
                    │  Test Data   │
                    │ (ITCH msgs)  │
                    └──────┬───────┘
                           │
              ┌────────────┴────────────┐
              │                         │
              ▼                         ▼
     ┌────────────────┐       ┌────────────────┐
     │  Golden Model  │       │  Verilator     │
     │  (C++ on CPU)  │       │  (RTL → C++)   │
     └───────┬────────┘       └───────┬────────┘
             │                        │
             ▼                        ▼
     ┌────────────────┐       ┌────────────────┐
     │ golden_trace   │       │ verilator_trace│
     │   .csv         │       │   .csv         │
     └───────┬────────┘       └───────┬────────┘
             │                        │
             └───────────┬────────────┘
                         │
                         ▼
                ┌────────────────┐
                │ verify_trace.py│
                │  (field-by-    │
                │   field diff)  │
                └───────┬────────┘
                        │
                        ▼
                   PASS / FAIL
```

### Three Levels of Verification

1. **Golden Model + Valgrind**: Proves the reference implementation is memory-safe (no leaks, no out-of-bounds access).

2. **Verilator Simulation**: Compiles SystemVerilog into C++ and runs a cycle-accurate simulation. The testbench feeds ITCH packets and checks parser outputs.

3. **Trace Comparison**: `verify_trace.py` compares golden and hardware traces field-by-field, proving bit-accuracy.

---

## Fixed-Point Arithmetic

### Why Not Floating Point?

Floating-point (`float`, `double`) on FPGAs is:
- **Slow**: FP multiply takes ~5 clock cycles vs 1 for fixed-point
- **Large**: FP units use ~5x more logic resources
- **Non-deterministic**: Rounding can vary between implementations

### How Fixed-Point Works

`ap_fixed<16, 8>` means:
- 16 total bits
- 8 integer bits (range: -128 to +127)
- 8 fractional bits (resolution: 1/256 ≈ 0.004)

```
Bit layout:  [S IIIIIII . FFFFFFFF]
              │ │       │ │
              │ │       │ └── 8 fractional bits
              │ │       └──── decimal point (implied)
              │ └──────────── 7 integer bits
              └────────────── 1 sign bit

Example: 3.75 in decimal
  = 3 + 0.5 + 0.25
  = 00000011.11000000 in binary
  = raw value: 0x03C0 = 960
  Verify: 960 / 256 = 3.75 ✓
```

### Multiplication

Fixed-point multiply: multiply the raw values, then shift right by FRAC_BITS.

```
  3.75 × 2.5
  = 960 × 640 = 614400
  614400 >> 8 = 2400
  2400 / 256 = 9.375 ✓
```

---

## AXI-Stream Protocol

AXI-Stream is the standard interface for streaming data on FPGAs. Think of it like a conveyor belt:

```
Signal      Description                     Analogy
──────      ───────────                     ───────
tdata       The data payload (64 bits)      The package on the belt
tvalid      "Data is ready"                 Green light: belt is moving
tready      "I can accept data"             Receiver says "I'm ready"
tlast       "This is the last beat"         End-of-packet marker
tkeep       Which bytes are valid           Which of the 8 bytes matter
```

**Handshake rule**: Data transfers only when `tvalid && tready` are both high on the same clock edge. This creates **backpressure** — if the receiver isn't ready, the sender waits.

In our design, `tready` is always 1 (no backpressure) because the parser must keep up with line rate. Dropping packets in trading would be catastrophic.

---

## Key Design Decisions

### 1. "Parse-as-you-go" vs. "Buffer-then-parse"
We chose parse-as-you-go: the FSM extracts fields from each AXI-Stream beat as it arrives, without buffering the full message. This minimizes latency (5 cycles vs. 5+N cycles) at the cost of more complex FSM logic.

### 2. Piecewise-linear sigmoid vs. LUT-based
The MoE softmax uses `sigmoid(x) ≈ 0.5 + 0.25x` for |x| < 2. This avoids both a lookup table (BRAM cost) and a Taylor series (compute cost). The approximation error is negligible for gating weights.

### 3. Top-2 experts (K=2)
With K=2, only 2 of 8 experts run per input. This means the compute cost scales with K, not N — critical for meeting the sub-microsecond budget. The two experts run in parallel on separate hardware.

### 4. BRAM-addressed LOB
Using price-as-address gives O(1) access but limits the price range to MAX_PRICE_LEVELS ticks. For a real instrument, this covers ~$40 at $0.01 tick size — sufficient for most liquid stocks.

### 5. Separate golden model (not just HLS csim)
The golden model is a standalone C++ program, not an HLS cosimulation. This means it can run without Xilinx tools, making CI/CD possible on standard GitHub Actions runners.

---

## Performance Analysis

### Latency Budget

| Stage | Cycles | Time @ 250MHz |
|-------|--------|---------------|
| ITCH Parser (5 beats) | 5 | 20 ns |
| MoE Router | 1 | 4 ns |
| Expert MLPs (2 parallel) | 1 | 4 ns |
| Weighted combination | 1 | 4 ns |
| Matching Engine | 1 | 4 ns |
| **Total pipeline** | **~10** | **~40 ns** |

### Throughput

At II=1 (initiation interval of 1 clock cycle), the pipeline can accept a new input every clock cycle once full. At 250MHz:
- **Sustained throughput**: 250M operations/sec per stage
- **Message throughput**: 250M/5 = 50M messages/sec (limited by 5-beat parser)

### Resource Estimates (approximate)

| Resource | ITCH Parser | MoE Router | Experts (×2) | Matching Engine |
|----------|-------------|------------|--------------|-----------------|
| LUTs | ~500 | ~2,000 | ~3,000 | ~1,000 |
| FFs | ~300 | ~1,000 | ~1,500 | ~500 |
| DSPs | 0 | 64 | 128 | 0 |
| BRAM | 0 | 2 | 4 | 8 |
