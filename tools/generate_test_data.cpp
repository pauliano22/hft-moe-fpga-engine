// =========================================================================
// generate_test_data.cpp — Synthetic NASDAQ ITCH 5.0 binary file generator
//
// Produces a binary file that conforms exactly to the ITCH 5.0 wire format:
//   [uint16_t length (big-endian)] [message body]
//
// The generated stream is statistically plausible but not market-accurate:
//   - Prices drift around a simulated mid-price with random walk
//   - Order reference numbers are assigned monotonically
//   - Executions, cancels, deletes, and replaces reference live orders
//
// Usage:
//   ./generate_test_data <output_file> [num_messages]
//
// Default: 1,000,000 messages written to the output file.
//
// No external dependencies — compiles with g++ -std=c++17 -O2
// =========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <random>

// -------------------------------------------------------------------------
// Portable big-endian write helpers
// -------------------------------------------------------------------------
static void w8(uint8_t* p, uint8_t v)  { p[0] = v; }

static void w16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

static void w32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
}

static void w64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

// Write a 48-bit timestamp (6 bytes, big-endian)
static void w48(uint8_t* p, uint64_t v) {
    for (int i = 5; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

// -------------------------------------------------------------------------
// Write a length-prefixed message to the output file
// -------------------------------------------------------------------------
static void write_msg(FILE* f, const uint8_t* body, uint16_t body_len) {
    uint8_t prefix[2];
    w16(prefix, body_len);
    fwrite(prefix, 1, 2, f);
    fwrite(body, 1, body_len, f);
}

// -------------------------------------------------------------------------
// Active order tracking — needed so E/X/D/U messages reference live orders
// -------------------------------------------------------------------------
struct ActiveOrder {
    uint64_t order_ref_num;
    char     side;          // 'B' or 'S'
    uint32_t price;
    uint32_t shares;
};

// -------------------------------------------------------------------------
// Message builders — each fills a fixed-size buffer and calls write_msg
// -------------------------------------------------------------------------

static void emit_system_event(FILE* f, uint64_t ts_ns, char event_code) {
    uint8_t buf[12];
    w8(buf +  0, 'S');
    w16(buf + 1, 1);            // stock locate
    w16(buf + 3, 0);            // tracking number
    w48(buf + 5, ts_ns);
    w8(buf + 11, static_cast<uint8_t>(event_code));
    write_msg(f, buf, 12);
}

static void emit_add_order(FILE* f, uint64_t ts_ns,
                             uint64_t order_ref_num, char side,
                             uint32_t shares, uint32_t price) {
    uint8_t buf[36];
    w8(buf  +  0, 'A');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    w8(buf  + 19, static_cast<uint8_t>(side));
    w32(buf + 20, shares);
    // Stock symbol: "AAPL    " (space-padded to 8 bytes)
    memcpy(buf + 24, "AAPL    ", 8);
    w32(buf + 32, price);
    write_msg(f, buf, 36);
}

static void emit_add_order_mpid(FILE* f, uint64_t ts_ns,
                                  uint64_t order_ref_num, char side,
                                  uint32_t shares, uint32_t price) {
    uint8_t buf[40];
    w8(buf  +  0, 'F');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    w8(buf  + 19, static_cast<uint8_t>(side));
    w32(buf + 20, shares);
    memcpy(buf + 24, "AAPL    ", 8);
    w32(buf + 32, price);
    memcpy(buf + 36, "GSCO", 4);   // market participant ID
    write_msg(f, buf, 40);
}

static void emit_order_executed(FILE* f, uint64_t ts_ns,
                                  uint64_t order_ref_num,
                                  uint32_t executed_shares,
                                  uint64_t match_number) {
    uint8_t buf[31];
    w8(buf  +  0, 'E');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    w32(buf + 19, executed_shares);
    w64(buf + 23, match_number);
    write_msg(f, buf, 31);
}

static void emit_order_cancel(FILE* f, uint64_t ts_ns,
                                uint64_t order_ref_num,
                                uint32_t cancelled_shares) {
    uint8_t buf[23];
    w8(buf  +  0, 'X');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    w32(buf + 19, cancelled_shares);
    write_msg(f, buf, 23);
}

static void emit_order_delete(FILE* f, uint64_t ts_ns,
                                uint64_t order_ref_num) {
    uint8_t buf[19];
    w8(buf  +  0, 'D');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    write_msg(f, buf, 19);
}

static void emit_order_replace(FILE* f, uint64_t ts_ns,
                                 uint64_t orig_ref, uint64_t new_ref,
                                 uint32_t shares, uint32_t price) {
    uint8_t buf[35];
    w8(buf  +  0, 'U');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, orig_ref);
    w64(buf + 19, new_ref);
    w32(buf + 27, shares);
    w32(buf + 31, price);
    write_msg(f, buf, 35);
}

static void emit_trade(FILE* f, uint64_t ts_ns,
                         uint64_t order_ref_num, char side,
                         uint32_t shares, uint32_t price,
                         uint64_t match_number) {
    uint8_t buf[44];
    w8(buf  +  0, 'P');
    w16(buf +  1, 1);
    w16(buf +  3, 0);
    w48(buf +  5, ts_ns);
    w64(buf + 11, order_ref_num);
    w8(buf  + 19, static_cast<uint8_t>(side));
    w32(buf + 20, shares);
    memcpy(buf + 24, "AAPL    ", 8);
    w32(buf + 32, price);
    w64(buf + 36, match_number);
    write_msg(f, buf, 44);
}

// =========================================================================
// main
// =========================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output_file> [num_messages]\n", argv[0]);
        return 1;
    }

    const char* out_path    = argv[1];
    long        target_msgs = (argc >= 3) ? atol(argv[2]) : 1000000L;

    FILE* f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", out_path);
        return 1;
    }

    // Seeded Mersenne Twister — reproducible output for a given seed
    std::mt19937_64 rng(12345678ULL);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      action_dist(0, 99);
    std::uniform_int_distribution<uint32_t> shares_dist(100, 10000);
    std::normal_distribution<double>        price_drift(0.0, 10.0); // ±$0.0010 drift per step

    // Simulate AAPL trading around $182.50 (price × 10000 = 1825000)
    double   mid_price    = 1825000.0;
    uint64_t order_ref    = 1ULL;         // monotonically increasing
    uint64_t match_number = 1ULL;
    uint64_t ts_ns        = 34200000000000ULL; // 09:30:00 in ns since midnight
    uint64_t ts_step      = 100ULL;           // 100 ns between messages

    // Pool of live orders — capped to avoid unbounded memory growth
    std::vector<ActiveOrder> live_orders;
    live_orders.reserve(100000);

    long msgs_written = 0;

    // Opening system event
    emit_system_event(f, ts_ns, 'Q'); // start market hours
    ++msgs_written;
    ts_ns += ts_step;

    while (msgs_written < target_msgs - 1) {
        ts_ns += ts_step;

        // Random walk: occasionally move the mid price
        if (msgs_written % 500 == 0) {
            mid_price += price_drift(rng);
            if (mid_price < 100000.0) mid_price = 100000.0; // floor at $10
        }

        int action = action_dist(rng);

        if (live_orders.size() < 100 || action < 55) {
            // --- Add Order (55% of messages when book has orders) ---
            char side = side_dist(rng) ? 'B' : 'S';
            // Spread: bid strictly below mid, ask strictly above.
            // Keep offsets consistent with mid_price direction so the book
            // doesn't cross when mid drifts slowly.
            double offset = (side == 'B') ? -(double)(100 + rng() % 500)
                                           :  (double)(100 + rng() % 500);
            uint32_t price  = static_cast<uint32_t>(
                std::max(1.0, mid_price + offset));
            uint32_t shares = shares_dist(rng);

            // Alternate between 'A' and 'F' message types
            if (msgs_written % 7 == 0) {
                emit_add_order_mpid(f, ts_ns, order_ref, side, shares, price);
            } else {
                emit_add_order(f, ts_ns, order_ref, side, shares, price);
            }

            live_orders.push_back({order_ref, side, price, shares});
            ++order_ref;
            ++msgs_written;

        } else if (action < 65 && !live_orders.empty()) {
            // --- Order Execute (10%) ---
            size_t idx = rng() % live_orders.size();
            ActiveOrder& ord = live_orders[idx];
            uint32_t exec_shares = std::min(ord.shares, shares_dist(rng));

            emit_order_executed(f, ts_ns, ord.order_ref_num, exec_shares, match_number++);
            ++msgs_written;

            ord.shares -= exec_shares;
            if (ord.shares == 0) {
                live_orders.erase(live_orders.begin() + idx);
            }

        } else if (action < 75 && !live_orders.empty()) {
            // --- Order Cancel (10%) ---
            size_t idx = rng() % live_orders.size();
            ActiveOrder& ord = live_orders[idx];
            uint32_t cancel_shares = std::min(ord.shares,
                                               shares_dist(rng) / 2 + 1);

            emit_order_cancel(f, ts_ns, ord.order_ref_num, cancel_shares);
            ++msgs_written;

            ord.shares -= cancel_shares;
            if (ord.shares == 0) {
                live_orders.erase(live_orders.begin() + idx);
            }

        } else if (action < 85 && !live_orders.empty()) {
            // --- Order Delete (10%) ---
            size_t idx = rng() % live_orders.size();
            emit_order_delete(f, ts_ns, live_orders[idx].order_ref_num);
            live_orders.erase(live_orders.begin() + idx);
            ++msgs_written;

        } else if (action < 90 && !live_orders.empty()) {
            // --- Order Replace (5%) ---
            size_t idx = rng() % live_orders.size();
            ActiveOrder& ord = live_orders[idx];
            uint64_t new_ref   = order_ref++;
            uint32_t new_price = static_cast<uint32_t>(
                std::max(1.0, mid_price + (rng() % 200) * (ord.side == 'B' ? -1.0 : 1.0)));
            uint32_t new_shares = shares_dist(rng);

            emit_order_replace(f, ts_ns, ord.order_ref_num, new_ref,
                                new_shares, new_price);
            ++msgs_written;

            // Update live pool: retire old ref, add new
            live_orders.erase(live_orders.begin() + idx);
            live_orders.push_back({new_ref, ord.side, new_price, new_shares});

        } else {
            // --- Non-Cross Trade (5%) ---
            char side = side_dist(rng) ? 'B' : 'S';
            uint32_t price  = static_cast<uint32_t>(std::max(1.0, mid_price));
            uint32_t shares = shares_dist(rng);
            emit_trade(f, ts_ns, order_ref++, side, shares, price, match_number++);
            ++msgs_written;
        }

        // Cap live order pool — emit proper Delete messages so the parser's
        // book stays consistent. Without this, the book accumulates phantom
        // orders that never get cleaned up, causing the pool and book to diverge.
        if (live_orders.size() > 50000 && msgs_written < target_msgs - 10001) {
            for (size_t k = 0; k < 10000 && msgs_written < target_msgs - 1; ++k) {
                ts_ns += ts_step;
                emit_order_delete(f, ts_ns, live_orders[k].order_ref_num);
                ++msgs_written;
            }
            live_orders.erase(live_orders.begin(), live_orders.begin() + 10000);
        }
    }

    // Closing system event
    emit_system_event(f, ts_ns, 'M'); // end market hours
    ++msgs_written;

    fclose(f);

    printf("Generated %ld messages -> %s\n", msgs_written, out_path);
    return 0;
}
