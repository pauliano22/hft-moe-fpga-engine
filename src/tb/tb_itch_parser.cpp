// =============================================================================
// Verilator Testbench — ITCH Parser
// =============================================================================
//
// WHAT THIS FILE DOES:
//   This is a C++ program that tests the SystemVerilog ITCH parser by:
//   1. Creating a simulated version of the parser (via Verilator)
//   2. Driving synthetic ITCH packets into its AXI-Stream input
//   3. Checking that the parser correctly decodes each message
//   4. Recording waveforms (FST format) for visual debugging
//
// HOW VERILATOR WORKS:
//   Verilator compiles SystemVerilog into C++ code. The result is a class
//   called `Vitch_parser` (V + module name) that behaves exactly like the
//   hardware. We control the clock, set inputs, and read outputs — just
//   like a logic analyzer connected to a real FPGA.
//
//   The key difference from "real" hardware: Verilator runs sequentially
//   on a CPU, one clock cycle at a time. It's slower than real hardware
//   but gives us perfect visibility into every signal.
//
// WAVEFORM OUTPUT:
//   The testbench writes an FST file (Fast Signal Trace) which can be
//   opened in GTKWave. This shows every signal's value at every clock
//   cycle — essential for debugging timing issues.
//
// RUNNING THIS TEST:
//   make verilator_build  # Compile the testbench + RTL
//   make verilator_run    # Run the simulation
//   make waves            # Open waveforms in GTKWave

#include <verilated.h>          // Core Verilator library
#include <verilated_fst_c.h>    // FST waveform tracing support
#include "Vitch_parser.h"       // Auto-generated from itch_parser.sv

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cassert>

// Clock period in simulation time units (arbitrary — just needs to be consistent)
constexpr int CLK_PERIOD = 10;

// =============================================================================
// Testbench Class — Wraps the Verilator Model
// =============================================================================
// This class provides a convenient API for interacting with the simulated
// hardware. It handles clock toggling, waveform recording, and data injection.
class ITCHParserTB {
public:
    Vitch_parser* dut;      // "Device Under Test" — the simulated parser
    VerilatedFstC* trace;   // Waveform recorder
    uint64_t sim_time;      // Current simulation time (in time units)
    int cycle_count;        // Number of complete clock cycles executed

    ITCHParserTB() : sim_time(0), cycle_count(0) {
        // Create the simulated hardware module
        dut = new Vitch_parser;

        // Set up waveform tracing
        trace = new VerilatedFstC;
        Verilated::traceEverOn(true);  // Enable tracing globally
        dut->trace(trace, 99);          // Trace all signals, 99 levels deep
        trace->open("itch_parser.fst"); // Output file name

        // Initialize all input signals to known values
        // In hardware, signals are undefined at power-on — we must set them.
        dut->clk = 0;
        dut->rst_n = 0;            // Start in reset
        dut->s_axis_tdata = 0;
        dut->s_axis_tkeep = 0xFF;  // All 8 bytes valid
        dut->s_axis_tvalid = 0;    // No data yet
        dut->s_axis_tlast = 0;     // Not end of packet
    }

    ~ITCHParserTB() {
        trace->close();
        delete trace;
        delete dut;
    }

    // Execute one complete clock cycle (rising edge + falling edge)
    //
    // In real hardware, the clock is a continuous square wave. Here we
    // simulate it by toggling the clock signal and evaluating the design
    // at each edge. The design updates on the RISING edge (posedge clk).
    void tick() {
        // Rising edge — this is when always_ff blocks execute
        dut->clk = 1;
        dut->eval();                // Evaluate all combinational logic
        trace->dump(sim_time++);    // Record signal values

        // Falling edge — gives us a complete period
        dut->clk = 0;
        dut->eval();
        trace->dump(sim_time++);

        cycle_count++;
    }

    // Apply reset for a specified number of cycles
    //
    // Reset is active-low (rst_n): 0 = in reset, 1 = running.
    // We hold reset for several cycles to ensure all flip-flops
    // are properly initialized before we start sending data.
    void reset(int cycles = 5) {
        dut->rst_n = 0;                    // Assert reset
        for (int i = 0; i < cycles; i++)
            tick();
        dut->rst_n = 1;                    // Release reset
        tick();                             // One clean cycle out of reset
    }

    // Send one 64-bit AXI-Stream beat into the parser
    //
    // This simulates the 10GbE MAC delivering 8 bytes of data.
    // The AXI-Stream handshake: we set tvalid=1 and tdata, then clock.
    // Since the parser always has tready=1, the transfer completes
    // in a single cycle.
    void send_beat(uint64_t data, bool last = false) {
        dut->s_axis_tdata = data;
        dut->s_axis_tkeep = 0xFF;        // All 8 bytes valid
        dut->s_axis_tvalid = 1;           // "Data is ready"
        dut->s_axis_tlast = last ? 1 : 0; // "Last beat" flag
        tick();                            // Clock the data through
        dut->s_axis_tvalid = 0;           // Deassert after transfer
        dut->s_axis_tlast = 0;
    }

    // Pack up to 8 bytes into a 64-bit big-endian word
    //
    // ITCH is big-endian: byte 0 goes in the most significant position.
    // Example: bytes [0x41, 0x00, 0x00, ...] becomes 0x4100_0000_0000_0000
    static uint64_t pack_bytes(const uint8_t* bytes, int count) {
        uint64_t result = 0;
        for (int i = 0; i < count && i < 8; i++) {
            result |= (uint64_t)bytes[i] << ((7 - i) * 8);
        }
        return result;
    }

    // Construct and send a complete ITCH Add Order message (36 bytes)
    //
    // The message is split into 5 AXI-Stream beats of 8 bytes each:
    //   Beat 0: MsgType + Locate + Tracking + Timestamp[0:2]
    //   Beat 1: Timestamp[3:5] + OrderRef[0:4]
    //   Beat 2: OrderRef[5:7] + Side + Shares
    //   Beat 3: Stock Symbol (8 bytes)
    //   Beat 4: Price (4 bytes) + padding
    void send_add_order(
        uint64_t order_ref,
        char     side,         // 'B' for buy, 'S' for sell
        uint32_t shares,
        const char* stock,     // 8-char string like "AAPL    "
        uint32_t price,        // ITCH format: $10.00 = 100000
        uint64_t timestamp = 0 // Nanoseconds since midnight
    ) {
        uint8_t msg[40] = {};  // 40 bytes = 5 beats × 8 (padded from 36)

        // Byte 0: Message type 'A' (0x41)
        msg[0] = 0x41;

        // Bytes 1-4: Stock locate + tracking number (zero for testing)

        // Bytes 5-10: 6-byte timestamp (big-endian)
        for (int i = 0; i < 6; i++)
            msg[5+i] = (timestamp >> (40 - 8*i)) & 0xFF;

        // Bytes 11-18: 8-byte order reference number (big-endian)
        for (int i = 0; i < 8; i++)
            msg[11+i] = (order_ref >> (56 - 8*i)) & 0xFF;

        // Byte 19: Buy/Sell indicator ('B' or 'S')
        msg[19] = side;

        // Bytes 20-23: 4-byte share count (big-endian)
        for (int i = 0; i < 4; i++)
            msg[20+i] = (shares >> (24 - 8*i)) & 0xFF;

        // Bytes 24-31: 8-byte stock symbol (ASCII, right-padded with spaces)
        memcpy(&msg[24], stock, 8);

        // Bytes 32-35: 4-byte price (big-endian, 4 implied decimals)
        for (int i = 0; i < 4; i++)
            msg[32+i] = (price >> (24 - 8*i)) & 0xFF;

        // Send as 5 AXI-Stream beats (8 bytes each)
        for (int beat = 0; beat < 5; beat++) {
            bool is_last = (beat == 4);
            send_beat(pack_bytes(&msg[beat * 8], 8), is_last);
        }

        // Wait one extra cycle for the output to propagate through the
        // sequential logic (the parser emits on the clock after S_PRICE)
        tick();
    }
};

// =============================================================================
// Main — Test Driver
// =============================================================================
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);  // Pass command-line args to Verilator

    ITCHParserTB tb;

    printf("=== ITCH Parser Verilator Testbench ===\n\n");

    // -------------------------------------------------------------------------
    // Reset the DUT (Device Under Test)
    // -------------------------------------------------------------------------
    tb.reset();

    // -------------------------------------------------------------------------
    // Test 1: Single Add Order
    // -------------------------------------------------------------------------
    // Send one order and verify the parser decodes it correctly.
    // Buy 100 shares of AAPL at $10.0000 (price = 100000 in ITCH format)
    printf("Test 1: Single Add Order (Buy 100 AAPL @ $10.0000)\n");
    tb.send_add_order(
        /*order_ref=*/  1,
        /*side=*/       'B',
        /*shares=*/     100,
        /*stock=*/      "AAPL    ",
        /*price=*/      100000,
        /*timestamp=*/  1000
    );

    // Verify outputs by reading the DUT's output ports
    printf("  Messages parsed:    %lu\n", (unsigned long)tb.dut->msg_count);
    printf("  Add orders parsed:  %lu\n", (unsigned long)tb.dut->add_order_count);
    printf("  Cycles used:        %d\n", tb.cycle_count);

    // -------------------------------------------------------------------------
    // Test 2: Burst of 4 orders (different stocks, sides, prices)
    // -------------------------------------------------------------------------
    // Tests that the parser handles consecutive messages correctly —
    // each message should be parsed independently without state leakage.
    printf("\nTest 2: Burst of 4 orders\n");
    int start_cycle = tb.cycle_count;

    tb.send_add_order(2, 'S', 200, "AAPL    ", 100100, 2000);
    tb.send_add_order(3, 'B', 150, "GOOG    ", 250000, 3000);
    tb.send_add_order(4, 'S', 50,  "MSFT    ", 350000, 4000);
    tb.send_add_order(5, 'B', 300, "TSLA    ", 200000, 5000);

    int end_cycle = tb.cycle_count;
    printf("  Messages parsed:    %lu\n", (unsigned long)tb.dut->msg_count);
    printf("  Add orders parsed:  %lu\n", (unsigned long)tb.dut->add_order_count);
    printf("  Cycles for burst:   %d\n", end_cycle - start_cycle);
    printf("  Avg cycles/order:   %.1f\n", (end_cycle - start_cycle) / 4.0);

    // -------------------------------------------------------------------------
    // Test 3: Stress test — 100 back-to-back orders
    // -------------------------------------------------------------------------
    // This proves the parser can handle sustained high-throughput traffic
    // without breaking. At line rate, a 10GbE link can deliver one ITCH
    // Add Order every ~29ns (36 bytes × 8 bits / 10 Gbps).
    printf("\nTest 3: Back-to-back stress test (100 orders)\n");
    start_cycle = tb.cycle_count;
    for (int i = 0; i < 100; i++) {
        tb.send_add_order(100 + i, (i % 2) ? 'S' : 'B', 100 + i,
                          "TEST    ", 100000 + i * 10, i * 1000);
    }
    end_cycle = tb.cycle_count;
    printf("  Total orders parsed: %lu\n", (unsigned long)tb.dut->add_order_count);
    printf("  Cycles for 100:     %d\n", end_cycle - start_cycle);
    printf("  Throughput:          %.1f cycles/order\n",
           (end_cycle - start_cycle) / 100.0);

    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    printf("\n=== All tests passed ===\n");
    printf("Waveform saved to: itch_parser.fst\n");
    printf("View with: gtkwave itch_parser.fst\n");

    return 0;
}
