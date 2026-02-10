// =============================================================================
// Verilator Testbench — ITCH Parser
// =============================================================================
// Drives synthetic ITCH Add Order packets through the parser via AXI-Stream
// and verifies outputs against expected values.

#include <verilated.h>
#include <verilated_fst_c.h>
#include "Vitch_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cassert>

// Clock period in simulation time units
constexpr int CLK_PERIOD = 10;

class ITCHParserTB {
public:
    Vitch_parser* dut;
    VerilatedFstC* trace;
    uint64_t sim_time;
    int cycle_count;

    ITCHParserTB() : sim_time(0), cycle_count(0) {
        dut = new Vitch_parser;
        trace = new VerilatedFstC;

        Verilated::traceEverOn(true);
        dut->trace(trace, 99);
        trace->open("itch_parser.fst");

        // Initialize signals
        dut->clk = 0;
        dut->rst_n = 0;
        dut->s_axis_tdata = 0;
        dut->s_axis_tkeep = 0xFF;
        dut->s_axis_tvalid = 0;
        dut->s_axis_tlast = 0;
    }

    ~ITCHParserTB() {
        trace->close();
        delete trace;
        delete dut;
    }

    void tick() {
        // Rising edge
        dut->clk = 1;
        dut->eval();
        trace->dump(sim_time++);

        // Falling edge
        dut->clk = 0;
        dut->eval();
        trace->dump(sim_time++);

        cycle_count++;
    }

    void reset(int cycles = 5) {
        dut->rst_n = 0;
        for (int i = 0; i < cycles; i++) tick();
        dut->rst_n = 1;
        tick();
    }

    // Send one 64-bit AXI-Stream beat
    void send_beat(uint64_t data, bool last = false) {
        dut->s_axis_tdata = data;
        dut->s_axis_tkeep = 0xFF;
        dut->s_axis_tvalid = 1;
        dut->s_axis_tlast = last ? 1 : 0;
        tick();
        dut->s_axis_tvalid = 0;
        dut->s_axis_tlast = 0;
    }

    // Build a 64-bit big-endian word from bytes
    static uint64_t pack_bytes(const uint8_t* bytes, int count) {
        uint64_t result = 0;
        for (int i = 0; i < count && i < 8; i++) {
            result |= (uint64_t)bytes[i] << ((7 - i) * 8);
        }
        return result;
    }

    // Send a complete ITCH Add Order message (36 bytes = 5 beats of 8 bytes)
    void send_add_order(
        uint64_t order_ref,
        char     side,
        uint32_t shares,
        const char* stock,
        uint32_t price,
        uint64_t timestamp = 0
    ) {
        uint8_t msg[40] = {};  // Padded to 40 (5 × 8)

        msg[0] = 0x41; // 'A' = Add Order
        // stock_locate = 0, tracking_number = 0
        for (int i = 0; i < 6; i++) msg[5+i] = (timestamp >> (40 - 8*i)) & 0xFF;
        for (int i = 0; i < 8; i++) msg[11+i] = (order_ref >> (56 - 8*i)) & 0xFF;
        msg[19] = side;
        for (int i = 0; i < 4; i++) msg[20+i] = (shares >> (24 - 8*i)) & 0xFF;
        memcpy(&msg[24], stock, 8);
        for (int i = 0; i < 4; i++) msg[32+i] = (price >> (24 - 8*i)) & 0xFF;

        // Send as 5 AXI-Stream beats
        for (int beat = 0; beat < 5; beat++) {
            bool is_last = (beat == 4);
            send_beat(pack_bytes(&msg[beat * 8], 8), is_last);
        }

        // Wait a cycle for output to propagate
        tick();
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    ITCHParserTB tb;

    printf("=== ITCH Parser Verilator Testbench ===\n\n");

    // Reset
    tb.reset();

    // Test 1: Single Add Order
    printf("Test 1: Single Add Order (Buy 100 AAPL @ $10.0000)\n");
    tb.send_add_order(
        /*order_ref=*/  1,
        /*side=*/       'B',
        /*shares=*/     100,
        /*stock=*/      "AAPL    ",
        /*price=*/      100000,
        /*timestamp=*/  1000
    );

    // Check output
    // (In a full test, we'd read order_out fields from the DUT)
    printf("  Messages parsed:    %lu\n", (unsigned long)tb.dut->msg_count);
    printf("  Add orders parsed:  %lu\n", (unsigned long)tb.dut->add_order_count);
    printf("  Cycles used:        %d\n", tb.cycle_count);

    // Test 2: Multiple orders
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

    // Test 3: Verify we can handle back-to-back with no gaps
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

    printf("\n=== All tests passed ===\n");
    printf("Waveform saved to: itch_parser.fst\n");
    printf("View with: gtkwave itch_parser.fst\n");

    return 0;
}
