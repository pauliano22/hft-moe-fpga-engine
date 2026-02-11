# Learning Guide

This guide tells you **which files to read, in what order**, to understand the entire project from scratch. Each section builds on the previous one.

**Time estimate**: ~2-3 hours for a thorough read-through.

---

## Prerequisites: What You Should Know

- **C++ basics** — variables, functions, structs, classes
- **Binary numbers** — bits, bytes, hex notation (0xFF)
- **Basic digital logic** — what a clock is, what flip-flops do (optional, we'll explain)

Don't worry if you don't know SystemVerilog, HLS, or FPGA concepts — this guide teaches them.

---

## Phase 1: Understand the Problem (15 min)

### Read: `README.md`
Start here. It explains:
- What the project does (trading engine on an FPGA)
- The architecture diagram (study this — it's the map for everything else)
- Performance targets (why we care about nanoseconds)
- The tech stack

### Read: `docs/ARCHITECTURE.md` — Sections 1-3 only
Read "The Big Picture", "Why FPGA?", and "Data Flow" sections. This gives you the mental model you need before diving into code.

**Key concepts to internalize:**
- Data flows left-to-right through a pipeline
- Each stage operates simultaneously (spatial parallelism)
- The FPGA processes 8 bytes every clock cycle (~4 nanoseconds)

---

## Phase 2: The Interface Language (20 min)

### Read: `src/rtl/axi_stream_pkg.sv`

This is the "dictionary" of the project. Before reading any hardware code, understand the types defined here:

1. **AXI-Stream signals** (lines 18-24): How data moves between stages. Focus on `tdata` (the data), `tvalid` (data is ready), `tlast` (end of packet).

2. **ITCH message types** (lines 29-39): The `0x41` = 'A' = Add Order code. The parser looks for this byte to identify messages.

3. **Parsed order struct** (lines 44-52): The parser's output — order reference, side (buy/sell), shares, stock symbol, price, timestamp.

4. **MoE types** (lines 57-76): Feature vector, trade signal. These define what the AI model sees and produces.

**New concept — `typedef struct packed`**: In SystemVerilog, `packed` means the struct is stored as contiguous bits, like a C struct with no padding. This matters because in hardware, we need to know exactly which wire carries which bit.

---

## Phase 3: The Hardware Parser (45 min)

### Read: `src/rtl/itch_parser.sv`

This is the most important file. Read it top-to-bottom:

1. **Header comments** (lines 1-21): Explains the ITCH Add Order message layout. **Study the byte offset table** — this is the specification the FSM implements.

2. **Module ports** (lines 23-44): Inputs from the network (`s_axis_*`), outputs to the next stage (`order_out`), and statistics counters.

3. **FSM states** (lines 49-57): Six states — IDLE, HEADER, ORDER_REF, SIDE_SHARES (folded into ORDER_REF), STOCK, PRICE, SKIP. Draw a state diagram on paper.

4. **`get_byte()` function** (lines 84-89): **Critical** — this is how you extract byte N from a 64-bit big-endian word. The formula `(7 - idx) * 8` inverts the byte index because ITCH is big-endian but AXI-Stream byte 0 is in the MSB.

5. **Sequential logic** (lines 94-195): The `always_ff` block runs on every clock edge. On reset, everything zeros out. On each valid beat, it accumulates fields byte-by-byte. **Trace through Test 1 from the testbench by hand** — pretend you're the hardware and process each beat.

6. **Combinational next-state** (lines 200-254): The `always_comb` block computes the next state purely based on current state and inputs. No clock — this is wires, not registers.

**New concept — `always_ff` vs `always_comb`**:
- `always_ff` = **registers** (stores values across clock cycles, like variables)
- `always_comb` = **wires** (computes values instantly, like a math formula)

### Read: `docs/ARCHITECTURE.md` — Section 4
Read the ITCH Parser deep-dive after the code to reinforce your understanding.

---

## Phase 4: The AI Model Concept (30 min)

### Read: `docs/ARCHITECTURE.md` — Sections 5-6
Read about MoE and Expert MLPs. Understand:
- Why MoE instead of a regular neural network
- What "top-K" means (only 2 of 8 experts activate)
- How the router decides which experts to use
- What ReLU activation does (max(0, x))

### Read: `src/hls/moe_router/moe_router.h`

Focus on the data structures:
- `FeatureVector`: 8 fixed-point numbers representing market state
- `RouterOutput`: Which 2 experts were selected + their weights
- `ExpertInput`: What gets sent to each expert
- `TradeSignal`: The final output (Buy/Sell/Hold + confidence)

**New concept — `ap_fixed<16,8>`**: Xilinx's fixed-point type. 16 total bits, 8 integer bits, 8 fractional bits. Range: -128 to +127.996. Resolution: ~0.004. See ARCHITECTURE.md Section 10 for details.

### Read: `src/hls/moe_router/moe_router.cpp`

Follow the algorithm:
1. Score each expert (matrix multiplication, lines 98-112)
2. Find top-2 experts (comparator, function `top_k_select`)
3. Compute gating weights (piecewise linear sigmoid)
4. Send features to the selected experts

**New concept — HLS pragmas**: Lines like `#pragma HLS PIPELINE II=1` are instructions to the HLS compiler:
- `PIPELINE II=1` → accept one new input every clock cycle
- `UNROLL` → replace the loop with parallel hardware (8 multipliers instead of 1 multiplier used 8 times)
- `ARRAY_PARTITION` → split an array across multiple BRAMs so they can all be read simultaneously

---

## Phase 5: The Expert Networks (15 min)

### Read: `src/hls/experts/expert_mlp.h` then `expert_mlp.cpp`

Each expert is a tiny 2-layer neural network:
- Layer 1: 8 inputs → 16 hidden neurons (with ReLU)
- Layer 2: 16 hidden → 1 output

The code is compact. Notice how `#pragma HLS UNROLL` on the inner loops means all 16 neurons compute simultaneously — this is why FPGAs can do neural network inference so fast.

---

## Phase 6: The Order Book (20 min)

### Read: `src/hls/matching_engine/matching_engine.h` then `matching_engine.cpp`

This implements the Limit Order Book (LOB) — the data structure that tracks all buy and sell orders.

**Key insight**: Price is used as a direct BRAM address. `lob.bid_levels[1500]` holds the quantity at price level 1500. No searching needed — O(1) access.

Follow the logic for a buy order:
1. Does the buy price ≥ best ask? (Does it "cross" the spread?)
2. If yes → match: fill the ask, reduce its quantity
3. If the ask level is now empty → scan for next best ask
4. Any remaining buy quantity → add to bid book

### Read: `docs/ARCHITECTURE.md` — Section 7
Reinforces the LOB concepts with diagrams.

---

## Phase 7: The Golden Model (30 min)

### Read: `src/golden_model/main.cpp`

This is the most readable file — pure C++ with no hardware concepts. It implements the **exact same algorithm** as the hardware, but using standard C++ data structures.

Read it in order:
1. **FixedPoint class** (lines 28-57): Emulates hardware fixed-point using `int16_t`
2. **ITCH parser** (lines 62-131): Parses binary ITCH messages byte-by-byte
3. **Feature extractor** (lines 149-181): Converts order + book state → 8 features
4. **MoE model** (lines 186-304): Router + experts + weighted combination
5. **Order book** (lines 310-371): LOB using `std::map`
6. **Main function** (lines 376-478): Drives the full pipeline with synthetic orders

**Exercise**: Run `make run_golden` and compare the console output to the code. Trace order #4 (a crossing buy) through all stages.

---

## Phase 8: The Testbench (15 min)

### Read: `src/tb/tb_itch_parser.cpp`

This is the **Verilator testbench** — a C++ program that drives the SystemVerilog parser. Verilator compiles the RTL into C++ code, and this testbench interacts with it.

Key parts:
- `ITCHParserTB` class: wraps the Verilator-generated `Vitch_parser` module
- `send_beat()`: drives one 64-bit AXI-Stream beat into the parser
- `send_add_order()`: constructs a full 36-byte ITCH message and sends it as 5 beats
- Three tests: single order, burst of 4, stress test of 100

**This is how we prove the hardware works** without a physical FPGA. Verilator simulates every clock cycle exactly as the hardware would behave.

---

## Phase 9: The Build System and CI (10 min)

### Read: `Makefile`

Understand the available targets:
- `make golden_model` → compile C++ golden model
- `make run_golden` → run it, generates `golden_trace.csv`
- `make verilator_build` → compile RTL into C++ simulator
- `make verilator_run` → run the simulator, generates waveform
- `make verify` → run both and compare
- `make validate` → Valgrind memory check

### Read: `.github/workflows/ci.yml`

Two CI jobs:
1. **golden-model**: build → run → Valgrind check → upload trace
2. **verilator-sim**: install Verilator → build → simulate → upload waveform

Both must pass green for the project to be considered working.

---

## Phase 10: Tools and Verification (10 min)

### Read: `tools/generate_itch_data.py`
Generates test data. Two modes: from a CSV of historical prices, or synthetic random walk. Outputs binary ITCH messages.

### Read: `sim/scripts/verify_trace.py`
The bit-accuracy checker. Compares golden and hardware traces field-by-field.

### Read: `sim/scripts/capture_waveform.sh`
Sets up GTKWave to view the FST waveform with the right signals pre-loaded.

---

## Suggested Exercises

After reading everything, try these to solidify your understanding:

1. **Trace by hand**: Take Test 1 from the testbench (Buy 100 AAPL @ $10.00) and manually compute what each FSM state does, what bytes get extracted from each beat.

2. **Modify the golden model**: Add a new test order and predict the output before running. Were you right?

3. **Change the expert count**: In `moe_router.h`, change `NUM_EXPERTS` from 8 to 4 and see what changes would be needed.

4. **Read the waveform**: If you have GTKWave, run `make waves` and identify the clock cycles where `order_out.valid` goes high.

5. **Break it on purpose**: Change a byte offset in `itch_parser.sv` and see if the testbench catches the error.

---

## Technology Glossary

| Term | Meaning |
|------|---------|
| **RTL** | Register Transfer Level — hardware described as registers and logic between them |
| **FSM** | Finite State Machine — a circuit that steps through defined states |
| **HLS** | High-Level Synthesis — compiler that turns C++ into hardware circuits |
| **Verilator** | Tool that compiles SystemVerilog into a C++ simulator |
| **AXI-Stream** | Standard protocol for streaming data between FPGA blocks |
| **BRAM** | Block RAM — fast on-chip memory built into FPGA fabric |
| **DSP** | Digital Signal Processor — hardware multiply-accumulate unit on FPGA |
| **LUT** | Look-Up Table — the basic logic element of an FPGA |
| **FF** | Flip-Flop — stores 1 bit, updates on clock edge |
| **II** | Initiation Interval — clock cycles between accepting new inputs |
| **LOB** | Limit Order Book — tracks all outstanding buy/sell orders |
| **ITCH** | NASDAQ's native binary market data protocol |
| **MoE** | Mixture of Experts — neural network with selective expert activation |
| **Top-K** | Selecting the K highest-scoring items from a set |
| **Pragma** | Compiler directive that hints at optimization strategy |
| **GTKWave** | Open-source waveform viewer for digital simulations |
| **FST** | Fast Signal Trace — compressed waveform file format |
| **Golden Model** | Trusted reference implementation for verification |
