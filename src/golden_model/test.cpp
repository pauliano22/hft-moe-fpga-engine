// =========================================================================
// test.cpp — Unit tests for the ITCH parser + order book
//
// Each test:
//   1. Constructs raw ITCH binary in a local buffer (exactly as it would
//      appear in a real Nasdaq feed, including the 2-byte length prefix)
//   2. Calls parse_message() directly to bypass file I/O
//   3. Verifies the resulting book state
//   4. Prints PASS or FAIL with a description
//
// Why test at the binary level?
//   The parser is the trust boundary — once parse_message() produces a
//   correct ParsedAddOrder, we know the endian conversion is right.
//   Testing at this level catches off-by-one errors in field offsets,
//   byte-swap bugs, and size mismatches between our structs and the spec.
// =========================================================================

#include "itch_parser.hpp"
#include "order_book.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// -------------------------------------------------------------------------
// Test framework — minimal PASS/FAIL reporting
// -------------------------------------------------------------------------
static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("  FAIL: %s  (line %d)\n", msg, __LINE__);             \
            return false;                                                  \
        }                                                                  \
    } while (0)

static void run_test(const char* name, bool (*fn)()) {
    ++tests_run;
    printf("[TEST %d] %s\n", tests_run, name);
    if (fn()) {
        ++tests_passed;
        printf("  PASS\n");
    }
    printf("\n");
}

// -------------------------------------------------------------------------
// Helper: write a 2-byte big-endian uint16 into a buffer
// -------------------------------------------------------------------------
static void write_be16(uint8_t* buf, uint16_t v) {
    buf[0] = static_cast<uint8_t>(v >> 8);
    buf[1] = static_cast<uint8_t>(v & 0xFF);
}

// Helper: write a 4-byte big-endian uint32 into a buffer
static void write_be32(uint8_t* buf, uint32_t v) {
    buf[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    buf[3] = static_cast<uint8_t>( v        & 0xFF);
}

// Helper: write an 8-byte big-endian uint64 into a buffer
static void write_be64(uint8_t* buf, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

// Helper: write a 6-byte big-endian uint48 timestamp into a buffer
static void write_be48(uint8_t* buf, uint64_t v) {
    for (int i = 5; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

// -------------------------------------------------------------------------
// build_add_order_msg — build a raw 'A' message body in a 36-byte buffer
//
// Fills all fields in big-endian byte order, exactly matching the wire
// format the ITCHParser expects.
// -------------------------------------------------------------------------
static void build_add_order_msg(uint8_t* buf,
                                 uint16_t stock_locate,
                                 uint64_t order_ref_num,
                                 char     side,
                                 uint32_t shares,
                                 const char* stock, // 8 chars, space-padded
                                 uint32_t price) {
    // Byte 0: message type
    buf[0] = 'A';
    // Bytes 1-2: stock locate
    write_be16(buf + 1, stock_locate);
    // Bytes 3-4: tracking number (set to 0 for tests)
    write_be16(buf + 3, 0);
    // Bytes 5-10: timestamp (set to fixed value for tests)
    write_be48(buf + 5, 34200000000000ULL); // 09:30:00.000 in ns
    // Bytes 11-18: order reference number
    write_be64(buf + 11, order_ref_num);
    // Byte 19: side
    buf[19] = static_cast<uint8_t>(side);
    // Bytes 20-23: shares
    write_be32(buf + 20, shares);
    // Bytes 24-31: stock symbol (8 bytes, space-padded)
    memcpy(buf + 24, stock, 8);
    // Bytes 32-35: price × 10000
    write_be32(buf + 32, price);
}

// -------------------------------------------------------------------------
// build_delete_msg — build a raw 'D' message body in a 19-byte buffer
// -------------------------------------------------------------------------
static void build_delete_msg(uint8_t* buf,
                               uint16_t stock_locate,
                               uint64_t order_ref_num) {
    buf[0] = 'D';
    write_be16(buf + 1, stock_locate);
    write_be16(buf + 3, 0);
    write_be48(buf + 5, 34200000000001ULL);
    write_be64(buf + 11, order_ref_num);
}

// -------------------------------------------------------------------------
// build_cancel_msg — build a raw 'X' message body in a 23-byte buffer
// -------------------------------------------------------------------------
static void build_cancel_msg(uint8_t* buf,
                               uint16_t stock_locate,
                               uint64_t order_ref_num,
                               uint32_t cancelled_shares) {
    buf[0] = 'X';
    write_be16(buf + 1, stock_locate);
    write_be16(buf + 3, 0);
    write_be48(buf + 5, 34200000000002ULL);
    write_be64(buf + 11, order_ref_num);
    write_be32(buf + 19, cancelled_shares);
}

// -------------------------------------------------------------------------
// build_execute_msg — build a raw 'E' message body in a 31-byte buffer
// -------------------------------------------------------------------------
static void build_execute_msg(uint8_t* buf,
                                uint16_t stock_locate,
                                uint64_t order_ref_num,
                                uint32_t executed_shares) {
    buf[0] = 'E';
    write_be16(buf + 1, stock_locate);
    write_be16(buf + 3, 0);
    write_be48(buf + 5, 34200000000003ULL);
    write_be64(buf + 11, order_ref_num);
    write_be32(buf + 19, executed_shares);
    write_be64(buf + 23, 999ULL); // match number
}

// =========================================================================
// TEST 1: Parse a known Add Order message, verify all fields
//
// This tests the parser's endian conversion in isolation. We craft a binary
// buffer with known field values, parse it, and check every field matches.
// If this test fails, the hardware will read corrupted data.
// =========================================================================
static bool test_parse_add_order_fields() {
    // Known field values
    const uint16_t STOCK_LOCATE  = 0x1234;
    const uint64_t ORDER_REF     = 0xDEADBEEFCAFEBABEULL;
    const char     SIDE          = 'B';
    const uint32_t SHARES        = 1500;
    const char     STOCK[9]      = "AAPL    "; // 8 wire bytes + null (for memcpy)
    const uint32_t PRICE         = 1820000;    // $182.0000

    uint8_t buf[36];
    build_add_order_msg(buf, STOCK_LOCATE, ORDER_REF, SIDE, SHARES, STOCK, PRICE);

    // Set up parser with a capture callback
    itch::ITCHParser  parser;
    itch::ParseStats  stats;
    itch::ParsedAddOrder received{};
    bool got_callback = false;

    parser.set_on_add_order([&](const itch::ParsedAddOrder& msg) {
        received = msg;
        got_callback = true;
    });

    bool dispatched = parser.parse_message(buf, 36, stats);

    CHECK(dispatched,      "parse_message returned false");
    CHECK(got_callback,    "on_add_order callback was not invoked");
    CHECK(stats.total_messages == 1, "total_messages should be 1");
    CHECK(stats.msg_type_counts['A'] == 1, "type count['A'] should be 1");

    CHECK(received.stock_locate  == STOCK_LOCATE,  "stock_locate mismatch");
    CHECK(received.order_ref_num == ORDER_REF,      "order_ref_num mismatch (endian bug?)");
    CHECK(received.side          == SIDE,           "side mismatch");
    CHECK(received.shares        == SHARES,         "shares mismatch");
    CHECK(received.price         == PRICE,          "price mismatch (endian bug?)");
    CHECK(received.has_mpid      == false,          "has_mpid should be false for type A");

    // Check stock ticker (8 wire bytes should be null-terminated to 9)
    CHECK(strncmp(received.stock, "AAPL    ", 8) == 0, "stock ticker mismatch");
    CHECK(received.stock[8] == '\0', "stock not null-terminated");

    // Verify timestamp — 09:30:00.000 encoded as nanoseconds
    CHECK(received.timestamp_ns == 34200000000000ULL, "timestamp mismatch");

    return true;
}

// =========================================================================
// TEST 2: Add an order then delete it — verify the book is empty
//
// This tests the full add→delete lifecycle. After a delete, the book should
// have zero orders, zero levels, and best_bid/best_ask should return 0.
// This is the most important correctness property: no phantom orders.
// =========================================================================
static bool test_add_then_delete_empty_book() {
    itch::ITCHParser parser;
    itch::ParseStats stats;
    book::OrderBook  ob;

    // Wire up parser → book
    parser.set_on_add_order     ([&](const itch::ParsedAddOrder& m)    { ob.add(m);    });
    parser.set_on_order_delete  ([&](const itch::ParsedOrderDelete& m)  { ob.remove(m); });

    const uint64_t ORDER_REF = 42ULL;
    const uint32_t PRICE     = 1000000; // $100.0000

    // Step 1: add a bid order
    uint8_t add_buf[36];
    build_add_order_msg(add_buf, 1, ORDER_REF, 'B', 100, "MSFT    ", PRICE);
    parser.parse_message(add_buf, 36, stats);

    CHECK(ob.order_count() == 1,         "should have 1 order after add");
    CHECK(ob.level_count() == 1,         "should have 1 bid level");
    CHECK(ob.best_bid()    == PRICE,     "best bid should match added price");
    CHECK(ob.best_ask()    == 0,         "ask side should be empty");

    // Step 2: delete the order
    uint8_t del_buf[19];
    build_delete_msg(del_buf, 1, ORDER_REF);
    parser.parse_message(del_buf, 19, stats);

    CHECK(ob.order_count() == 0,         "book should be empty after delete");
    CHECK(ob.level_count() == 0,         "no price levels should remain");
    CHECK(ob.best_bid()    == 0,         "best_bid should return 0 on empty book");
    CHECK(ob.best_ask()    == 0,         "best_ask should return 0 on empty book");
    CHECK(ob.spread()      == 0,         "spread should be 0 on empty book");

    return true;
}

// =========================================================================
// TEST 3: Build a two-sided book, verify spread, mid-price, and OIR
//
// Adds multiple orders on each side to exercise:
//   - Price-level aggregation (multiple orders at same price)
//   - best_bid < best_ask (crossed book detection)
//   - spread() = best_ask - best_bid
//   - mid_price() = (best_bid + best_ask) / 2
//   - order_imbalance_ratio() sign and direction
//
// Also tests partial cancel: reduce shares on a live order, verify level
// total_shares updates correctly.
// =========================================================================
static bool test_two_sided_book_and_features() {
    itch::ITCHParser parser;
    itch::ParseStats stats;
    book::OrderBook  ob;

    parser.set_on_add_order    ([&](const itch::ParsedAddOrder& m)    { ob.add(m);    });
    parser.set_on_order_cancel ([&](const itch::ParsedOrderCancel& m)  { ob.cancel(m); });
    parser.set_on_order_delete ([&](const itch::ParsedOrderDelete& m)  { ob.remove(m); });
    parser.set_on_order_executed([&](const itch::ParsedOrderExecuted& m){ ob.execute(m);});

    // --- Build bid side ---
    // Order 1: bid $99.90, 200 shares (order_ref=1)
    // Order 2: bid $99.90, 100 shares (same level, order_ref=2)
    // Order 3: bid $99.80, 500 shares (order_ref=3)
    const uint32_t BID_HIGH = 999000; // $99.9000 × 10000
    const uint32_t BID_LOW  = 998000; // $99.8000 × 10000

    uint8_t buf[44]; // max message size we'll use

    build_add_order_msg(buf, 1, 1ULL, 'B', 200, "GOOG    ", BID_HIGH);
    parser.parse_message(buf, 36, stats);
    build_add_order_msg(buf, 1, 2ULL, 'B', 100, "GOOG    ", BID_HIGH);
    parser.parse_message(buf, 36, stats);
    build_add_order_msg(buf, 1, 3ULL, 'B', 500, "GOOG    ", BID_LOW);
    parser.parse_message(buf, 36, stats);

    // --- Build ask side ---
    // Order 4: ask $100.10, 300 shares (order_ref=4)
    // Order 5: ask $100.20, 150 shares (order_ref=5)
    const uint32_t ASK_NEAR = 1001000; // $100.1000 × 10000
    const uint32_t ASK_FAR  = 1002000; // $100.2000 × 10000

    build_add_order_msg(buf, 1, 4ULL, 'S', 300, "GOOG    ", ASK_NEAR);
    parser.parse_message(buf, 36, stats);
    build_add_order_msg(buf, 1, 5ULL, 'S', 150, "GOOG    ", ASK_FAR);
    parser.parse_message(buf, 36, stats);

    // Verify basic book structure
    CHECK(ob.order_count() == 5,          "should have 5 active orders");
    CHECK(ob.best_bid()    == BID_HIGH,   "best bid should be highest bid price");
    CHECK(ob.best_ask()    == ASK_NEAR,   "best ask should be lowest ask price");

    // Verify there is no crossed book (essential invariant)
    CHECK(ob.best_bid() < ob.best_ask(),  "crossed book: best bid must be < best ask");

    // Spread = ASK_NEAR - BID_HIGH = 1001000 - 999000 = 2000 = $0.2000
    CHECK(ob.spread() == 2000,            "spread mismatch");

    // Mid = (999000 + 1001000) / 2 = 1000000 = $100.0000
    CHECK(ob.mid_price() == 1000000,      "mid price mismatch");

    // OIR: bid_qty (top 5) = 200+100+500 = 800
    //       ask_qty (top 5) = 300+150 = 450
    //       OIR = (800 - 450) / (800 + 450) = 350/1250 = 0.28
    double oir = ob.order_imbalance_ratio(5);
    CHECK(oir > 0.0,   "OIR should be positive (bid-heavy)");
    CHECK(oir < 1.0,   "OIR should be < 1.0");
    // Check approximate value: 0.28 ± 0.01
    CHECK(oir > 0.27 && oir < 0.29, "OIR value out of expected range");

    // --- Test partial cancel ---
    // Cancel 100 shares from order 1 (was 200 shares at BID_HIGH)
    // After cancel: order 1 has 100 shares; BID_HIGH total = 100+100 = 200
    build_cancel_msg(buf, 1, 1ULL, 100);
    parser.parse_message(buf, 23, stats);

    CHECK(ob.order_count() == 5, "partial cancel should not remove the order");

    book::BookSnapshot snap = ob.top_of_book(5);
    CHECK(!snap.bids.empty(),                      "bids should not be empty after cancel");
    CHECK(snap.bids[0].price == BID_HIGH,          "best bid price unchanged after partial cancel");
    // Level total should now be 100 (order1 remaining) + 100 (order2) = 200
    CHECK(snap.bids[0].total_shares == 200,        "bid level total_shares after partial cancel");
    CHECK(snap.bids[0].order_count == 2,           "still 2 orders at BID_HIGH level");

    // --- Test full execution (order 4, 300 shares) ---
    build_execute_msg(buf, 1, 4ULL, 300);
    parser.parse_message(buf, 31, stats);

    CHECK(ob.order_count() == 4, "fully executed order should be removed");
    // Best ask should now be ASK_FAR
    CHECK(ob.best_ask() == ASK_FAR, "best ask should advance after order 4 executes");

    return true;
}

// =========================================================================
// main
// =========================================================================
int main() {
    printf("=== GOLDEN MODEL UNIT TESTS ===\n\n");

    run_test("Parse Add Order — verify all fields (endian, offsets)", test_parse_add_order_fields);
    run_test("Add then Delete — verify book is empty",                 test_add_then_delete_empty_book);
    run_test("Two-sided book — spread, mid-price, OIR, cancel, exec",  test_two_sided_book_and_features);

    printf("=== RESULTS: %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
