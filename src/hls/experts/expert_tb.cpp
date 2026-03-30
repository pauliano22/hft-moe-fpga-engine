// =========================================================================
// expert_tb.cpp — C-simulation testbench for the HLS Expert Kernels
// =========================================================================

#include "expert_kernel.hpp"
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

// Build a sample FeatureVector
static FeatureVector make_features(double oir, double spread_norm) {
    FeatureVector f{};
    f.mid_price_norm   = 0.25;
    f.spread_norm      = spread_norm;
    f.order_imbalance  = oir;
    f.bid_qty_norm     = 0.5;
    f.ask_qty_norm     = 0.3;
    f.bid_ask_ratio    = 1.67;
    f.spread_to_mid    = 0.001;
    f.price_velocity   = 0.0002;
    return f;
}

// =========================================================================
// TEST 1: Forward pass produces finite output
// =========================================================================
static bool test_forward_finite() {
    FeatureVector f = make_features(0.3, 2.0);

    for (int eid = 0; eid < N_EXPERTS; ++eid) {
        fixed_t out = expert_forward(f, eid);
        double d = static_cast<double>(out);
        // Output should be finite and within reasonable range
        char msg[64];
        snprintf(msg, sizeof(msg), "expert %d output should be finite", eid);
        CHECK(d > -100.0 && d < 100.0, msg);
    }
    return true;
}

// =========================================================================
// TEST 2: Different inputs produce different outputs
// =========================================================================
static bool test_different_inputs_different_outputs() {
    FeatureVector f_buy  = make_features( 0.8, 0.5);  // strong bid pressure
    FeatureVector f_sell = make_features(-0.8, 0.5);  // strong ask pressure

    // At least one expert should differentiate between the two
    bool any_diff = false;
    for (int eid = 0; eid < N_EXPERTS; ++eid) {
        double out_buy  = static_cast<double>(expert_forward(f_buy,  eid));
        double out_sell = static_cast<double>(expert_forward(f_sell, eid));
        if (fabs(out_buy - out_sell) > 1e-6) any_diff = true;
    }
    CHECK(any_diff, "at least one expert should produce different outputs for different inputs");
    return true;
}

// =========================================================================
// TEST 3: Full pipeline run_expert + combine_experts
// =========================================================================
static bool test_full_pipeline() {
    const int N = 5;

    // Build RouterOutput messages
    hls::stream<RouterOutput> in0, in1;
    hls::stream<ExpertOutput> out0, out1;
    hls::stream<TradeDecision> decisions;

    FeatureVector f = make_features(0.4, 1.5);

    for (int i = 0; i < N; ++i) {
        RouterOutput r{};
        r.expert_id[0]   = 0;
        r.expert_id[1]   = 1;
        r.gate_weight[0] = 0.6;
        r.gate_weight[1] = 0.4;
        r.features       = f;
        r.msg_type       = 'A';
        in0.write(r);
        in1.write(r);
    }

    run_expert(in0, out0, N, 0);  // expert 0
    run_expert(in1, out1, N, 1);  // expert 1

    CHECK(out0.size() == N, "expert 0 should emit N outputs");
    CHECK(out1.size() == N, "expert 1 should emit N outputs");

    combine_experts(out0, out1, decisions, N);

    CHECK(decisions.size() == N, "combiner should emit N decisions");

    for (int i = 0; i < N; ++i) {
        TradeDecision d = decisions.read();
        // trade_signal must be 0, 1, or 2
        CHECK(d.trade_signal <= 2, "trade_signal must be 0 (HOLD), 1 (BUY), or 2 (SELL)");
    }

    return true;
}

// =========================================================================
// TEST 4: Weighted output is bounded
// =========================================================================
static bool test_weighted_output_bounded() {
    const int N = 20;
    hls::stream<RouterOutput> in0, in1;
    hls::stream<ExpertOutput> out0, out1;
    hls::stream<TradeDecision> decisions;

    for (int i = 0; i < N; ++i) {
        RouterOutput r{};
        r.expert_id[0]   = 2;
        r.expert_id[1]   = 3;
        r.gate_weight[0] = 0.7;
        r.gate_weight[1] = 0.3;
        r.features       = make_features((i % 2 == 0) ? 0.5 : -0.5, 1.0);
        r.msg_type       = 'A';
        in0.write(r);
        in1.write(r);
    }

    run_expert(in0, out0, N, 2);
    run_expert(in1, out1, N, 3);
    combine_experts(out0, out1, decisions, N);

    for (int i = 0; i < N; ++i) {
        TradeDecision d = decisions.read();
        double wo = static_cast<double>(d.weighted_output);
        CHECK(wo > -50.0 && wo < 50.0, "weighted output should be bounded");
    }

    return true;
}

int main() {
    printf("=== EXPERT KERNEL C-SIMULATION TESTBENCH ===\n\n");
    run_test("Forward pass produces finite output",           test_forward_finite);
    run_test("Different inputs → different outputs",          test_different_inputs_different_outputs);
    run_test("Full pipeline: run_expert + combine_experts",   test_full_pipeline);
    run_test("Weighted output is bounded",                     test_weighted_output_bounded);

    printf("=== RESULTS: %d / %d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
