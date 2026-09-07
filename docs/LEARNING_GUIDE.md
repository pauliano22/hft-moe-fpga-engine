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

This is a conceptual "dictionary" for the project's types — note that it's a reference file only: `itch_parser.sv`/`order_book.sv`/`top.sv` use plain `logic` ports rather than importing these structs (see the file's own header comment). Still, understand the field layouts here before reading the real RTL:

1. **AXI-Stream signals** (`axis_master_t`, lines 53-60): How data moves between stages. Focus on `tdata` (the data), `tvalid` (data is ready), `tlast` (end of packet).

2. **ITCH message types** (`itch_msg_type_t`, lines 71-81): The `8'h41` = 'A' = Add Order code. The parser looks for this byte to identify messages.

3. **Parsed order struct** (`parsed_add_order_t`, lines 97-105): Illustrates the parser's output — order reference, side (buy/sell), shares, stock symbol, price, timestamp. The actual `itch_parser.sv` exposes these as individual `m_axis_*` ports instead (and doesn't forward a timestamp).

4. **MoE types** (`moe_input_t` / `trade_signal_t`, lines 127-146): Feature vector, trade signal. The real feature list used by the router is `FeatureVector` in `moe_router.hpp`, not the placeholder one implied here.

**New concept — `typedef struct packed`**: In SystemVerilog, `packed` means the struct is stored as contiguous bits, like a C struct with no padding. This matters because in hardware, we need to know exactly which wire carries which bit.

---

## Phase 3: The Hardware Parser (45 min)

### Read: `src/rtl/itch_parser.sv`

This is the most important file. Read it top-to-bottom:

1. **Header comments** (lines 1-30): Explains the ITCH Add Order message layout and the beat-by-beat parse plan. **Study the byte offset table** — this is the specification the FSM implements.

2. **Module ports** (lines 32-54): Inputs from the network (`s_axis_*`), outputs to the next stage (`m_axis_*`), and the stock/order/side/shares/price fields the order book consumes.

3. **FSM states** (lines 59-67): Seven states — IDLE, PARSE_B0, PARSE_B1, PARSE_B2, PARSE_B3, PARSE_B4, EMIT — one state per 8-byte AXI-Stream beat, plus IDLE and a one-cycle EMIT. Draw a state diagram on paper.

4. **Byte extraction** (line 92): **Critical** — there's no separate `get_byte()` function; a `` `BYTE(beat_data, offset) `` macro extracts byte N within a beat via `beat_data[(offset*8) +: 8]`. TDATA[7:0] holds byte 0 (the first byte to arrive on the wire), so no big-endian/little-endian inversion is needed.

5. **Sequential logic** (lines 103-254): The two `always_ff` blocks run on every clock edge. On reset, everything zeros out. On each valid beat, it accumulates fields byte-by-byte using the `` `BYTE `` macro. **Trace through Test 1 from the testbench by hand** — pretend you're the hardware and process each beat.

6. **Combinational next-state** (lines 261-328): The `always_comb` block computes the next state purely based on current state and inputs. No clock — this is wires, not registers.

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
- What "top-K" means (only 2 of 4 experts activate)
- How the router decides which experts to use
- What ReLU activation does (max(0, x))

### Read: `src/hls/moe_router/moe_router.hpp`

Focus on the data structures:
- `FeatureVector`: 8 fixed-point numbers representing market state
- `RouterOutput`: Which 2 experts were selected + their weights

**New concept — `ap_fixed<16,6>`**: Xilinx's fixed-point type. 16 total bits, 6 integer bits, 10 fractional bits. Range: -32 to +31.999. Resolution: ~0.001. See ARCHITECTURE.md Section 10 for details.

### Read: `src/hls/moe_router/moe_router.cpp`

Follow the algorithm:
1. Score each expert (matrix multiplication, lines 95-104)
2. Find top-2 experts (comparator, `TOP_K_SELECT` loop starting at line 115)
3. Compute gating weights (piecewise linear sigmoid)
4. Send features to the selected experts

**New concept — HLS pragmas**: Lines like `#pragma HLS PIPELINE II=1` are instructions to the HLS compiler:
- `PIPELINE II=1` → accept one new input every clock cycle
- `UNROLL` → replace the loop with parallel hardware (8 multipliers instead of 1 multiplier used 8 times)
- `ARRAY_PARTITION` → split an array across multiple BRAMs so they can all be read simultaneously

---

## Phase 5: The Expert Networks (15 min)

### Read: `src/hls/experts/expert_kernel.hpp` then `expert_kernel.cpp`

Each expert is a tiny 2-layer neural network:
- Layer 1: 8 inputs → 16 hidden neurons (with ReLU)
- Layer 2: 16 hidden → 1 output

The code is compact. Notice how `#pragma HLS UNROLL` on the inner loops means all 16 neurons compute simultaneously — this is why FPGAs can do neural network inference so fast.

---

## Phase 6: The Order Book (20 min)

### Read: `src/hls/matching_engine/lob.hpp` then `lob.cpp`

This implements the Limit Order Book (LOB) — the data structure that tracks all buy and sell orders.

**Key insight**: Price is used as a direct BRAM address. `bid_shares[price_to_idx(price)]` holds the quantity at that price level (index 0 = `BASE_PRICE`, up to `MAX_PRICE_LEVELS - 1`). No searching needed — O(1) access.

Follow the logic for a buy order:
1. Does the buy price ≥ best ask? (Does it "cross" the spread?)
2. If yes → match: fill the ask, reduce its quantity
3. If the ask level is now empty → scan for next best ask
4. Any remaining buy quantity → add to bid book

### Read: `docs/ARCHITECTURE.md` — Section 7
Reinforces the LOB concepts with diagrams.

---

## Phase 7: The Golden Model (30 min)

### Read: `src/golden_model/itch_parser.{hpp,cpp}`, `order_book.{hpp,cpp}`, then `main.cpp`

This is the most readable code in the repo — pure C++ with no hardware concepts. It implements the **exact same ITCH-parsing and order-book algorithm** as the hardware (`src/rtl/itch_parser.sv` and `src/hls/matching_engine/lob.cpp`), but using standard C++ data structures. (MoE router/expert parity is covered separately by the HLS C-simulation testbenches from Phase 4-5 — the golden model here only covers parsing + book state, matching the "Golden Model Units" row in the README's Verification Strategy table.)

Read it in order:
1. **`itch_parser.hpp`**: The per-message-type structs (`AddOrderMsg`, `OrderExecutedMsg`, ...) and the `ITCHParser` class, which dispatches parsed messages via callbacks (`set_on_add_order`, etc.)
2. **`itch_parser.cpp`**: `parse_file()` / `parse_message()` read the length-prefixed binary stream and dispatch to per-type `dispatch_*()` functions that decode fields byte-by-byte
3. **`order_book.hpp`**: The `OrderBook` class — bids/asks are kept in `std::map<price, PriceLevel>` (descending for bids, ascending for asks), giving O(log n) best-bid/ask instead of hardware's O(1) array lookup
4. **`order_book.cpp`**: `add()`, `execute()`, `cancel()`, `remove()`, `replace()` mutate the book; `best_bid()`/`best_ask()`/`top_of_book()` read it back
5. **`main.cpp`**: The CLI driver — wires `ITCHParser` callbacks to `OrderBook` mutators, times the run, and prints benchmark/book-state output

**Exercise**: Run `cd src/golden_model && make run` and compare the console output to the code. Trace order #4 (a crossing buy) through all stages.

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

### Read: the per-directory Makefiles

There is no single top-level build; each stage has its own `Makefile` (see the README's "Build & Run" section for the exact commands):
- `src/golden_model/Makefile` → `make test-data` (generate synthetic ITCH data), `make test` (build + run unit tests), `make bench` (build + run throughput benchmark)
- `src/hls/Makefile` → `make` (compile all HLS testbenches with g++), `make test` (run all 13 tests)
- `sim/verilator/Makefile` → `make` (compile `top.sv` into a C++ simulator), then run `./obj_dir/Vtop --file ../../data/sample.itch ...` directly, or `make waves` to open the last VCD in GTKWave

### Read: `.github/workflows/ci.yml`

Three CI jobs:
1. **golden-model**: build → run unit tests → benchmark → Valgrind check
2. **hls-csim**: compile the HLS modules with g++ → run the C-simulation testbenches
3. **verilator**: install Verilator → build RTL sim → simulate → upload latency plots/CSV

All three must pass green for the project to be considered working.

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

3. **Change the expert count**: In `moe_router.hpp`, change `N_EXPERTS` from 4 to 8 and see what changes would be needed.

4. **Read the waveform**: If you have GTKWave, run `cd sim/verilator && make run && make waves` and identify the clock cycles where the parser's `m_axis_tvalid` (on `u_itch_parser`) and the top-level `book_valid` go high.

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
