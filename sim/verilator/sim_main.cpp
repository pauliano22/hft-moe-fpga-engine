// =========================================================================
// sim_main.cpp — Verilator C++ Simulation Harness
//
// Reads the synthetic ITCH binary from data/sample.itch, feeds raw bytes
// into the DUT (top.sv) 8 bytes per clock cycle (simulating 64-bit AXI-
// Stream at 250 MHz = 2 GB/s), and measures per-message latency.
//
// Key metrics reported:
//   - Ingress cycle: when s_axis_tvalid first goes high for a new message
//   - Egress cycle:  when book_valid asserts after that message
//   - Latency in cycles and nanoseconds (4 ns per cycle at 250 MHz)
//   - p50/p95/p99/max latency over all messages
//   - Total throughput: messages/sec (simulation time)
//
// The simulation also:
//   - Dumps a VCD waveform to sim/verilator/waves.vcd (open in GTKWave)
//   - Compares parsed fields against golden model output
// =========================================================================

#include "Vtop.h"                    // Verilator-generated DUT class
#include "verilated.h"
#include "verilated_vcd_c.h"         // VCD waveform writer

// Golden model for comparison
#include "../../src/golden_model/itch_parser.hpp"
#include "../../src/golden_model/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <vector>

// Simulation clock period in nanoseconds (250 MHz → 4 ns)
static const double CLK_PERIOD_NS = 4.0;

// -------------------------------------------------------------------------
// Simple high-resolution wall-clock timer (for throughput measurement)
// -------------------------------------------------------------------------
#include <time.h>
static uint64_t wall_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

// -------------------------------------------------------------------------
// Run one clock cycle: toggle clock, evaluate, optionally trace
// -------------------------------------------------------------------------
static void tick(Vtop* dut, VerilatedVcdC* vcd, uint64_t& sim_time) {
    // Rising edge
    dut->clk = 1;
    dut->eval();
    if (vcd) vcd->dump(sim_time);
    sim_time += 2;

    // Falling edge
    dut->clk = 0;
    dut->eval();
    if (vcd) vcd->dump(sim_time);
    sim_time += 2;
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char** argv) {
    // Default input file
    const char* itch_file = "../../data/sample.itch";
    const char* vcd_file  = "waves.vcd";
    bool        enable_vcd = true;
    int         max_msgs   = -1;  // -1 = process all

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file")  == 0 && i+1 < argc) itch_file = argv[++i];
        if (strcmp(argv[i], "--waves") == 0 && i+1 < argc) vcd_file  = argv[++i];
        if (strcmp(argv[i], "--no-vcd")== 0) enable_vcd = false;
        if (strcmp(argv[i], "--limit") == 0 && i+1 < argc) max_msgs  = atoi(argv[++i]);
    }

    // ------------------------------------------------------------------
    // Initialize Verilator
    // ------------------------------------------------------------------
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(enable_vcd);

    Vtop* dut = new Vtop;

    VerilatedVcdC* vcd = nullptr;
    if (enable_vcd) {
        vcd = new VerilatedVcdC;
        dut->trace(vcd, 99);     // trace all signals, depth 99
        vcd->open(vcd_file);
        printf("VCD waveform: %s (open with GTKWave)\n", vcd_file);
    }

    uint64_t sim_time = 0;

    // Reset
    dut->rst_n = 0;
    dut->s_axis_tvalid = 0;
    dut->s_axis_tdata  = 0;
    dut->s_axis_tkeep  = 0;
    dut->s_axis_tlast  = 0;
    // m_axis_tready is internal (order_book.msg_ready is hardwired to 1 in order_book.sv)

    for (int i = 0; i < 4; ++i) tick(dut, vcd, sim_time);
    dut->rst_n = 1;
    for (int i = 0; i < 2; ++i) tick(dut, vcd, sim_time);

    // ------------------------------------------------------------------
    // Load ITCH file into memory
    // ------------------------------------------------------------------
    FILE* f = fopen(itch_file, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", itch_file);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file_buf(file_size);
    (void)fread(file_buf.data(), 1, file_size, f);
    fclose(f);

    printf("Loaded %ld bytes from %s\n", file_size, itch_file);

    // ------------------------------------------------------------------
    // Set up golden model for comparison
    // ------------------------------------------------------------------
    itch::ITCHParser  gm_parser;
    book::OrderBook   gm_book;
    itch::ParseStats  gm_stats;
    int               gm_match = 0, gm_mismatch = 0;

    gm_parser.set_on_add_order    ([&](const itch::ParsedAddOrder& m)    { gm_book.add(m);    });
    gm_parser.set_on_order_delete ([&](const itch::ParsedOrderDelete& m)  { gm_book.remove(m); });
    gm_parser.set_on_order_executed([&](const itch::ParsedOrderExecuted& m){ gm_book.execute(m);});
    gm_parser.set_on_order_cancel ([&](const itch::ParsedOrderCancel& m)  { gm_book.cancel(m); });

    // ------------------------------------------------------------------
    // Simulation loop: feed 8 bytes per clock
    // ------------------------------------------------------------------
    std::vector<double> latencies_ns;
    latencies_ns.reserve(100000);

    const uint8_t* ptr       = file_buf.data();
    const uint8_t* end       = file_buf.data() + file_size;
    int            msg_count = 0;
    uint64_t       t_wall_start = wall_ns();

    // Track ingress cycle for latency measurement
    uint64_t ingress_cycle   = 0;
    bool     measuring       = false;
    uint32_t prev_best_bid   = 0;

    while (ptr < end) {
        // Read length prefix (2 bytes big-endian)
        if (ptr + 2 > end) break;
        uint16_t msg_len = (static_cast<uint16_t>(ptr[0]) << 8) | ptr[1];
        if (ptr + 2 + msg_len > end) break;

        // Total bytes for this message (length prefix + body)
        uint32_t total_bytes = 2 + msg_len;
        uint32_t num_beats   = (total_bytes + 7) / 8;

        // Run golden model on this message (for comparison)
        gm_parser.parse_message(ptr + 2, msg_len, gm_stats);
        uint32_t gold_bid = gm_book.best_bid();

        // Record ingress time
        ingress_cycle = sim_time / 4;  // convert ns to cycles
        measuring     = true;

        // Feed beats into DUT
        for (uint32_t beat = 0; beat < num_beats; ++beat) {
            uint64_t data = 0;
            uint8_t  keep = 0;

            for (int b = 0; b < 8; ++b) {
                uint32_t offset = beat * 8 + b;
                if (offset < total_bytes) {
                    data |= static_cast<uint64_t>(ptr[offset]) << (b * 8);
                    keep |= (1 << b);
                }
            }

            bool is_last = (beat == num_beats - 1);

            dut->s_axis_tvalid = 1;
            dut->s_axis_tdata  = data;
            dut->s_axis_tkeep  = keep;
            dut->s_axis_tlast  = is_last ? 1 : 0;

            // Settle combinatorial logic (next_state) with new inputs BEFORE
            // the posedge. Without this, Verilator's _sequent function runs
            // first (assigning state ← old_next_state), then _combo updates
            // next_state — meaning the state register lags one cycle.
            dut->eval();

            // Wait for tready before advancing
            do {
                tick(dut, vcd, sim_time);
            } while (!dut->s_axis_tready);
        }

        // Deassert valid after the last beat
        dut->s_axis_tvalid = 0;
        dut->s_axis_tlast  = 0;

        // Run a few more cycles to let the pipeline complete
        for (int i = 0; i < 8; ++i) {
            tick(dut, vcd, sim_time);

            // Check if book_valid asserted (egress event)
            if (measuring && dut->book_valid && dut->best_bid != 0) {
                uint64_t egress_cycle = sim_time / 4;
                double   lat_ns = static_cast<double>(egress_cycle - ingress_cycle)
                                   * CLK_PERIOD_NS;
                latencies_ns.push_back(lat_ns);
                measuring = false;

                // Compare DUT best_bid against golden model
                if (dut->best_bid == gold_bid) {
                    ++gm_match;
                } else {
                    ++gm_mismatch;
                }
            }
        }

        ptr += total_bytes;
        ++msg_count;

        if (max_msgs > 0 && msg_count >= max_msgs) break;

        // Limit VCD size: stop tracing after 10000 messages to keep file manageable
        if (vcd && msg_count == 10000) {
            vcd->close();
            delete vcd;
            vcd = nullptr;
            printf("VCD closed at %d messages (file size limit)\n", msg_count);
        }
    }

    uint64_t t_wall_end = wall_ns();
    double   wall_sec   = static_cast<double>(t_wall_end - t_wall_start) / 1e9;

    // ------------------------------------------------------------------
    // Compute latency statistics
    // ------------------------------------------------------------------
    printf("\n=== VERILATOR SIMULATION RESULTS ===\n");
    printf("  Messages processed:  %d\n", msg_count);
    printf("  Simulation wall time: %.3f s\n", wall_sec);
    printf("  Throughput:          %.2f M sim-msg/s\n",
           static_cast<double>(msg_count) / wall_sec / 1e6);
    printf("  Golden model matches:    %d\n", gm_match);
    printf("  Golden model mismatches: %d\n", gm_mismatch);

    if (!latencies_ns.empty()) {
        std::sort(latencies_ns.begin(), latencies_ns.end());
        size_t n = latencies_ns.size();
        double p50  = latencies_ns[n * 50  / 100];
        double p95  = latencies_ns[n * 95  / 100];
        double p99  = latencies_ns[n * 99  / 100];
        double pmax = latencies_ns.back();

        printf("\n  Wire-to-response latency (cycles × 4 ns @ 250 MHz):\n");
        printf("    p50:  %.1f ns  (%.0f cycles)\n", p50,  p50  / CLK_PERIOD_NS);
        printf("    p95:  %.1f ns  (%.0f cycles)\n", p95,  p95  / CLK_PERIOD_NS);
        printf("    p99:  %.1f ns  (%.0f cycles)\n", p99,  p99  / CLK_PERIOD_NS);
        printf("    max:  %.1f ns  (%.0f cycles)\n", pmax, pmax / CLK_PERIOD_NS);
        printf("\n  NOTE: Latency includes only itch_parser + order_book.\n");
        printf("        Add ~32 ns (8 cycles) for HLS MoE inference.\n");
    }

    // Write latency CSV for plotting
    {
        FILE* csv = fopen("latencies.csv", "w");
        if (csv) {
            fprintf(csv, "message_index,latency_ns\n");
            for (size_t i = 0; i < latencies_ns.size(); ++i)
                fprintf(csv, "%zu,%.1f\n", i, latencies_ns[i]);
            fclose(csv);
            printf("\n  Latency data: latencies.csv\n");
        }
    }

    // Cleanup
    if (vcd) { vcd->close(); delete vcd; }
    dut->final();
    delete dut;

    return 0;
}
