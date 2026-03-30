// =========================================================================
// main.cpp — Golden model CLI
//
// Usage:
//   ./golden_model --file data/sample.itch [--bench] [--verify N] [--top N]
//
// Flags:
//   --file PATH    ITCH 5.0 binary file to parse (required)
//   --bench        Print throughput and per-type message counts
//   --verify N     Print the order book state every N messages
//   --top N        Print top-N price levels after the full run (default 5)
// =========================================================================

#include "itch_parser.hpp"
#include "order_book.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

// -------------------------------------------------------------------------
// Portable nanosecond timer
//
// Why CLOCK_MONOTONIC_RAW on Linux?
//   It is not slewed by NTP — once we start a measurement, the clock ticks
//   at a fixed hardware rate, giving stable latency numbers even when the
//   system is under NTP adjustment. CLOCK_MONOTONIC can drift ±500 ppm.
//   CLOCK_MONOTONIC_RAW is Linux-only; we fall back to CLOCK_MONOTONIC on
//   other platforms (macOS, Windows/MinGW).
// -------------------------------------------------------------------------
static uint64_t now_ns() {
#if defined(_WIN32)
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return static_cast<uint64_t>(count.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
#  ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#  else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#  endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

// -------------------------------------------------------------------------
// print_usage
// -------------------------------------------------------------------------
static void print_usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s --file PATH [--bench] [--verify N] [--top N]\n"
        "\n"
        "  --file PATH    ITCH 5.0 binary file to process (required)\n"
        "  --bench        Report throughput and per-type message counts\n"
        "  --verify N     Print book state every N messages (0 = off)\n"
        "  --top N        Print top-N book levels at end of run (default 5)\n"
        "\n"
        "Example:\n"
        "  %s --file data/sample.itch --bench --top 10\n",
        argv0, argv0);
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // --- Argument parsing (no getopt — it behaves differently on Windows) ---
    std::string file_path;
    bool        do_bench  = false;
    int         verify_n  = 0;
    int         top_n     = 5;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_path = argv[++i];
        } else if (strcmp(argv[i], "--bench") == 0) {
            do_bench = true;
        } else if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (file_path.empty()) {
        fprintf(stderr, "Error: --file is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // --- Set up the parser and order book ---
    itch::ITCHParser parser;
    book::OrderBook  ob;
    itch::ParseStats stats;

    uint64_t msg_counter = 0;  // local counter for --verify

    // Wire parser callbacks to order book mutators.
    // Each lambda captures ob and msg_counter by reference so they share state.
    parser.set_on_add_order([&](const itch::ParsedAddOrder& msg) {
        ob.add(msg);
        ++msg_counter;
        if (verify_n > 0 && msg_counter % static_cast<uint64_t>(verify_n) == 0) {
            printf("\n--- Book state after %lu messages ---\n",
                   (unsigned long)msg_counter);
            ob.print(top_n);
        }
    });

    parser.set_on_order_executed([&](const itch::ParsedOrderExecuted& msg) {
        ob.execute(msg);
        ++msg_counter;
    });

    parser.set_on_order_cancel([&](const itch::ParsedOrderCancel& msg) {
        ob.cancel(msg);
        ++msg_counter;
    });

    parser.set_on_order_delete([&](const itch::ParsedOrderDelete& msg) {
        ob.remove(msg);
        ++msg_counter;
    });

    parser.set_on_order_replace([&](const itch::ParsedOrderReplace& msg) {
        ob.replace(msg);
        ++msg_counter;
    });

    // Trades don't affect the resting book — just count them
    parser.set_on_trade([&](const itch::ParsedTrade&) {
        ++msg_counter;
    });

    parser.set_on_system_event([&](const itch::ParsedSystemEvent& msg) {
        if (do_bench) {
            printf("[SystemEvent] event_code='%c' ts=%lu ns\n",
                   msg.event_code, (unsigned long)msg.timestamp_ns);
        }
    });

    // --- Time the parse + book-update pass ---
    printf("Processing: %s\n", file_path.c_str());
    uint64_t t_start = now_ns();

    bool ok = parser.parse_file(file_path, stats);

    uint64_t t_end = now_ns();

    if (!ok) {
        fprintf(stderr, "Error: failed to open or read '%s'\n", file_path.c_str());
        return 1;
    }

    double elapsed_sec = static_cast<double>(t_end - t_start) / 1e9;

    // --- Benchmark report ---
    if (do_bench) {
        printf("\n=== BENCHMARK RESULTS ===\n");
        printf("  File:              %s\n", file_path.c_str());
        printf("  Bytes read:        %llu\n",
               (unsigned long long)stats.bytes_read);
        printf("  Total messages:    %llu\n",
               (unsigned long long)stats.total_messages);
        printf("  Parse errors:      %llu\n",
               (unsigned long long)stats.parse_errors);
        printf("  Elapsed time:      %.6f s\n", elapsed_sec);

        if (elapsed_sec > 0.0) {
            double msg_per_sec = static_cast<double>(stats.total_messages) / elapsed_sec;
            printf("  Throughput:        %.2f M msg/s\n", msg_per_sec / 1e6);
            printf("  Ns per message:    %.1f ns/msg\n",
                   (elapsed_sec * 1e9) / static_cast<double>(stats.total_messages));
        }

        printf("\n  Per-type breakdown:\n");
        // Ordered list of known types for consistent output
        const char types[] = "SAFEXDUPRHYLVWKJCQBINsaferupdcb";
        for (char t : std::string(types)) {
            auto it = stats.msg_type_counts.find(t);
            if (it != stats.msg_type_counts.end() && it->second > 0) {
                printf("    [%c] %-24s %llu\n",
                       t,
                       itch::ITCHParser::msg_type_name(t),
                       (unsigned long long)it->second);
            }
        }
        // Print any types not in our known list
        for (auto& [t, count] : stats.msg_type_counts) {
            if (strchr(types, t) == nullptr && count > 0) {
                printf("    [%c] %-24s %llu\n",
                       t,
                       itch::ITCHParser::msg_type_name(t),
                       (unsigned long long)count);
            }
        }
    }

    // --- Final book state ---
    printf("\n=== FINAL BOOK STATE ===\n");
    ob.print(top_n);

    return stats.parse_errors > 0 ? 1 : 0;
}
