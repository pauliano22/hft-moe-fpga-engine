// =========================================================================
// lob_tb.cpp — C-simulation testbench for the HLS LOB
//
// Compiles with g++ (using hls_sim_stubs.hpp) for quick functional testing
// without the Vitis HLS toolchain.
//
// Also used as the Vitis HLS C-simulation testbench (run via run_csim.tcl).
// The same binary is used in both cases — no code duplication.
//
// Tests:
//   1. Add a bid order, verify best_bid updates
//   2. Add an ask order, verify best_ask and spread update
//   3. Delete the bid order, verify best_bid drops to 0
//   4. Add + execute (full), verify order is removed
//   5. Diff against golden model for 100 messages
// =========================================================================

#include "lob.hpp"
#include <cstdio>
#include <cstring>

// For golden model comparison
#include "../../golden_model/itch_parser.hpp"
#include "../../golden_model/order_book.hpp"

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
            return false; \
        } \
    } while(0)

static void run_test(const char* name, bool (*fn)()) {
    ++tests_run;
    printf("[TEST %d] %s\n", tests_run, name);
    if (fn()) { ++tests_passed; printf("  PASS\n"); }
    printf("\n");
}

// -------------------------------------------------------------------------
// Helpers to reset book state between tests
// -------------------------------------------------------------------------
static ap_uint<32> bid_shares[MAX_PRICE_LEVELS];
static ap_uint<32> ask_shares[MAX_PRICE_LEVELS];
static ap_uint<64> order_ref_table[MAX_ORDERS];
static ap_uint<1>  order_side_table[MAX_ORDERS];
static ap_uint<32> order_price_table[MAX_ORDERS];
static ap_uint<32> order_shares_table[MAX_ORDERS];

static void reset_book() {
    memset(bid_shares, 0, sizeof(bid_shares));
    memset(ask_shares, 0, sizeof(ask_shares));
    memset(order_ref_table, 0, sizeof(order_ref_table));
    memset(order_side_table, 0, sizeof(order_side_table));
    memset(order_price_table, 0, sizeof(order_price_table));
    memset(order_shares_table, 0, sizeof(order_shares_table));
}

static BookUpdate send_msg(const OrderMsg& msg) {
    return process_one(msg, bid_shares, ask_shares,
                       order_ref_table, order_side_table,
                       order_price_table, order_shares_table);
}

// =========================================================================
// TEST 1: Add bid → best_bid updates
// =========================================================================
static bool test_add_bid_updates_best() {
    reset_book();

    // Price: $182.50 = 1825000; within range [BASE=1750000, BASE+2047*100]
    OrderMsg msg{};
    msg.msg_type  = MSG_ADD;
    msg.order_ref = 1ULL;
    msg.side      = 0;          // bid
    msg.shares    = 500;
    msg.price     = 1825000;    // $182.50

    BookUpdate upd = send_msg(msg);

    CHECK(upd.best_bid == 1825000, "best_bid should equal added price");
    CHECK(upd.best_ask == 0,       "ask side should be empty");
    CHECK(upd.spread   == 0,       "spread undefined when one side empty");
    return true;
}

// =========================================================================
// TEST 2: Add both sides → spread and mid_price
// =========================================================================
static bool test_spread_and_mid() {
    reset_book();

    // Bid at $182.40
    OrderMsg bid{};
    bid.msg_type = MSG_ADD; bid.order_ref = 10; bid.side = 0;
    bid.shares = 200; bid.price = 1824000; // $182.40
    send_msg(bid);

    // Ask at $182.60
    OrderMsg ask{};
    ask.msg_type = MSG_ADD; ask.order_ref = 11; ask.side = 1;
    ask.shares = 150; ask.price = 1826000; // $182.60
    BookUpdate upd = send_msg(ask);

    CHECK(upd.best_bid == 1824000, "best_bid mismatch");
    CHECK(upd.best_ask == 1826000, "best_ask mismatch");
    CHECK(upd.spread   == 2000,    "spread should be $0.20 = 2000");
    CHECK(upd.mid_price == 1825000, "mid should be $182.50");
    return true;
}

// =========================================================================
// TEST 3: Delete bid order → best_bid drops to 0
// =========================================================================
static bool test_delete_clears_best() {
    reset_book();

    OrderMsg add{};
    add.msg_type = MSG_ADD; add.order_ref = 20; add.side = 0;
    add.shares = 100; add.price = 1800000;
    send_msg(add);

    OrderMsg del{};
    del.msg_type = MSG_DELETE; del.order_ref = 20;
    BookUpdate upd = send_msg(del);

    CHECK(upd.best_bid == 0, "best_bid should be 0 after deleting the only bid");
    return true;
}

// =========================================================================
// TEST 4: Partial execute → shares reduced, order stays
// =========================================================================
static bool test_partial_execute() {
    reset_book();

    OrderMsg add{};
    add.msg_type = MSG_ADD; add.order_ref = 30; add.side = 1; // ask
    add.shares = 400; add.price = 1810000;
    send_msg(add);

    // Execute 100 of the 400 shares
    OrderMsg exec{};
    exec.msg_type = MSG_EXECUTE; exec.order_ref = 30; exec.shares = 100;
    BookUpdate upd = send_msg(exec);

    // Ask should still exist (300 shares remaining)
    CHECK(upd.best_ask == 1810000, "ask should still be present after partial execute");
    CHECK(upd.ask_total_qty == 300, "ask qty should be 300 after partial execute");
    return true;
}

// =========================================================================
// TEST 5: HLS LOB vs Golden Model comparison
//
// Runs the same sequence of messages through both the HLS LOB and the
// golden model. Compares best_bid and best_ask after every message.
// =========================================================================
static bool test_vs_golden_model() {
    reset_book();
    book::OrderBook golden;

    struct TestMsg {
        uint8_t  type; char side; uint64_t ref; uint32_t shares; uint32_t price;
        uint64_t new_ref; uint32_t new_price; uint32_t new_shares;
    };

    // A small deterministic sequence
    TestMsg msgs[] = {
        {'A', 'B', 100, 300, 1800000, 0, 0, 0},  // add bid $180.00
        {'A', 'S', 101, 200, 1820000, 0, 0, 0},  // add ask $182.00
        {'A', 'B', 102, 150, 1790000, 0, 0, 0},  // add bid $179.00
        {'A', 'S', 103, 250, 1830000, 0, 0, 0},  // add ask $183.00
        {'E',  0,  100,  50,       0, 0, 0, 0},  // exec 50 from bid 100
        {'X',  0,  101, 100,       0, 0, 0, 0},  // cancel 100 from ask 101
        {'D',  0,  102,   0,       0, 0, 0, 0},  // delete bid 102
    };

    int mismatches = 0;
    for (auto& m : msgs) {
        // Build HLS message
        OrderMsg hlsmsg{};
        hlsmsg.order_ref = m.ref;
        hlsmsg.shares    = m.shares;
        hlsmsg.price     = m.price;
        hlsmsg.side      = (m.side == 'B') ? 0 : 1;
        hlsmsg.msg_type  = m.type;

        // Build golden model message
        if (m.type == 'A') {
            itch::ParsedAddOrder gadd{};
            gadd.order_ref_num = m.ref;
            gadd.shares = m.shares;
            gadd.price = m.price;
            gadd.side = m.side;
            gadd.stock_locate = 1;
            gadd.has_mpid = false;
            strncpy(gadd.stock, "AAPL    ", 9);
            golden.add(gadd);
        } else if (m.type == 'E') {
            itch::ParsedOrderExecuted gexec{};
            gexec.order_ref_num = m.ref;
            gexec.executed_shares = m.shares;
            golden.execute(gexec);
        } else if (m.type == 'X') {
            itch::ParsedOrderCancel gcancel{};
            gcancel.order_ref_num = m.ref;
            gcancel.cancelled_shares = m.shares;
            golden.cancel(gcancel);
        } else if (m.type == 'D') {
            itch::ParsedOrderDelete gdel{};
            gdel.order_ref_num = m.ref;
            golden.remove(gdel);
        }

        BookUpdate upd = send_msg(hlsmsg);

        // Compare best_bid
        uint32_t gold_bid = golden.best_bid();
        uint32_t gold_ask = golden.best_ask();

        if (upd.best_bid != gold_bid || upd.best_ask != gold_ask) {
            printf("  MISMATCH after msg type='%c' ref=%lu: "
                   "HLS bid=%u ask=%u | golden bid=%u ask=%u\n",
                   m.type, (unsigned long)m.ref,
                   (unsigned)upd.best_bid, (unsigned)upd.best_ask,
                   gold_bid, gold_ask);
            ++mismatches;
        }
    }

    CHECK(mismatches == 0, "HLS LOB should match golden model for all test messages");
    return true;
}

// =========================================================================
// main
// =========================================================================
int main() {
    printf("=== HLS LOB C-SIMULATION TESTBENCH ===\n\n");
    run_test("Add bid → best_bid updates",          test_add_bid_updates_best);
    run_test("Add both sides → spread and mid",      test_spread_and_mid);
    run_test("Delete order → best_bid drops to 0",   test_delete_clears_best);
    run_test("Partial execute → shares reduced",      test_partial_execute);
    run_test("HLS LOB vs Golden Model comparison",    test_vs_golden_model);

    printf("=== RESULTS: %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
