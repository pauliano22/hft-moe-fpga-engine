// =========================================================================
// moe_router_tb.cpp — C-simulation testbench for the HLS MoE Router
// =========================================================================

#include "moe_router.hpp"
#include <cstdio>
#include <cmath>

static int tests_run = 0, tests_passed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); return false; } } while(0)

static void run_test(const char* name, bool (*fn)()) {
    ++tests_run;
    printf("[TEST %d] %s\n", tests_run, name);
    if (fn()) { ++tests_passed; printf("  PASS\n"); }
    printf("\n");
}

// =========================================================================
// TEST 1: Feature extraction produces in-range values
// =========================================================================
static bool test_feature_range() {
    BookUpdate b{};
    b.best_bid      = 1824000;  // $182.40
    b.best_ask      = 1826000;  // $182.60
    b.spread        = 2000;
    b.mid_price     = 1825000;  // $182.50
    b.bid_total_qty = 5000;
    b.ask_total_qty = 3000;
    b.msg_type      = 'A';

    FeatureVector f = extract_features(b, 1824500); // prev mid = $182.45

    // All features should be in fixed_t range [-32, 32)
    CHECK(f.mid_price_norm    > -32.0 && f.mid_price_norm    < 32.0, "mid_price_norm out of range");
    CHECK(f.spread_norm       >= 0.0,                                 "spread_norm should be non-negative");
    CHECK(f.order_imbalance   >= -1.0 && f.order_imbalance   <= 1.0, "OIR out of [-1,1]");
    CHECK(f.bid_qty_norm      >= 0.0,                                 "bid_qty_norm should be non-negative");
    CHECK(f.ask_qty_norm      >= 0.0,                                 "ask_qty_norm should be non-negative");
    CHECK(f.bid_ask_ratio     >= -4.0 && f.bid_ask_ratio     <= 4.0, "bid_ask_ratio out of clamp range");

    // OIR should be positive (bid-heavy: 5000 bid > 3000 ask)
    CHECK(f.order_imbalance > 0.0, "OIR should be positive (bid-heavy book)");

    // Spread-to-mid should be very small (tight spread)
    CHECK(f.spread_to_mid > 0.0 && f.spread_to_mid < 0.01, "spread_to_mid seems wrong");

    return true;
}

// =========================================================================
// TEST 2: Gate weights sum to 1.0 and expert IDs are valid
// =========================================================================
static bool test_gate_weights_sum_to_one() {
    FeatureVector f{};
    f.order_imbalance = 0.5;   // bid-heavy
    f.spread_norm     = 2.0;

    RouterOutput r = gate_and_select(f, 'A');

    // Expert IDs must be in [0, N_EXPERTS)
    CHECK(r.expert_id[0] < (ap_uint<4>)N_EXPERTS, "expert_id[0] out of range");
    CHECK(r.expert_id[1] < (ap_uint<4>)N_EXPERTS, "expert_id[1] out of range");

    // The two selected experts must be different
    CHECK(r.expert_id[0] != r.expert_id[1], "selected experts should be distinct");

    // Weights should sum to ~1.0
    double w_sum = static_cast<double>(r.gate_weight[0]) +
                   static_cast<double>(r.gate_weight[1]);
    CHECK(fabs(w_sum - 1.0) < 0.05, "gate weights should sum to ~1.0");

    // Both weights should be positive
    CHECK(r.gate_weight[0] > 0.0, "gate_weight[0] should be positive");
    CHECK(r.gate_weight[1] > 0.0, "gate_weight[1] should be positive");

    return true;
}

// =========================================================================
// TEST 3: Stream-based route_message processes N messages
// =========================================================================
static bool test_stream_routing() {
    hls::stream<BookUpdate>    book_in;
    hls::stream<RouterOutput>  router_out;

    const int N = 10;
    for (int i = 0; i < N; ++i) {
        BookUpdate b{};
        b.best_bid = 1820000 + i * 100;
        b.best_ask = 1822000 + i * 100;
        b.spread   = 2000;
        b.mid_price = 1821000 + i * 100;
        b.bid_total_qty = 1000 + i * 50;
        b.ask_total_qty = 800  + i * 30;
        b.msg_type = 'A';
        book_in.write(b);
    }

    route_message(book_in, router_out, N);

    CHECK(router_out.size() == N, "should have N outputs");

    for (int i = 0; i < N; ++i) {
        RouterOutput r = router_out.read();
        double wsum = static_cast<double>(r.gate_weight[0]) +
                      static_cast<double>(r.gate_weight[1]);
        CHECK(fabs(wsum - 1.0) < 0.1, "gate weights should sum to ~1.0");
    }

    return true;
}

int main() {
    printf("=== MoE ROUTER C-SIMULATION TESTBENCH ===\n\n");
    run_test("Feature extraction produces in-range values", test_feature_range);
    run_test("Gate weights sum to 1.0, IDs valid",          test_gate_weights_sum_to_one);
    run_test("Stream-based routing, N messages",             test_stream_routing);

    printf("=== RESULTS: %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
