// =========================================================================
// expert_kernel.cpp — HLS MLP Expert Kernel implementation
// =========================================================================

#ifdef __SYNTHESIS__
  #include <ap_int.h>
  #include <ap_fixed.h>
  #include <hls_stream.h>
#else
  #include "../include/hls_sim_stubs.hpp"
#endif

#include "expert_kernel.hpp"

// =========================================================================
// Weight tables — one set per expert, stored as compile-time constants.
//
// These are random weights for demo purposes. In production they would be
// trained via backprop and loaded from DRAM via AXI-MM at startup.
//
// In synthesis, constant arrays declared with 'const' are synthesized as
// ROM implemented in either:
//   - Distributed RAM (LUTs): for small tables (< ~512 bits)
//   - Block RAM (BRAM):       for larger tables (> ~512 bits)
// Our weight tables: 4 experts × (8×16 + 16×1) weights × 16 bits
//                  = 4 × 144 × 16 = 9216 bits ≈ one BRAM18.
//
// Indexed as W1[expert_id][hidden][input], W2[expert_id][output][hidden].
// =========================================================================

// Layer 1 weights: [N_EXPERTS][HIDDEN_DIM][INPUT_DIM]
static const fixed_t W1[4][16][8] = {
  // Expert 0
  {{ 0.3125,  0.1875, -0.4375,  0.2500,  0.3750, -0.1250,  0.4375, -0.3125},
   {-0.2500,  0.4375,  0.1875, -0.3125,  0.2500,  0.4375, -0.3750,  0.1250},
   { 0.4375, -0.3125,  0.2500,  0.3750, -0.1875, -0.4375,  0.3125,  0.2500},
   {-0.1875,  0.2500, -0.4375,  0.1875,  0.3125, -0.2500,  0.4375, -0.3750},
   { 0.3750, -0.1875,  0.3125, -0.4375,  0.1875,  0.3750, -0.2500,  0.4375},
   {-0.4375,  0.3125,  0.2500, -0.1875,  0.4375, -0.3750,  0.1875, -0.3125},
   { 0.1875, -0.4375,  0.3750,  0.2500, -0.3125,  0.1875,  0.4375, -0.2500},
   {-0.3750,  0.2500, -0.1875,  0.4375,  0.2500, -0.3125,  0.1875,  0.3125},
   { 0.2500,  0.3125, -0.3750, -0.2500,  0.4375,  0.1875, -0.4375,  0.3750},
   {-0.4375, -0.1875,  0.3125,  0.3750, -0.2500,  0.4375,  0.2500, -0.1875},
   { 0.3125,  0.4375, -0.2500, -0.3125,  0.1875, -0.3750,  0.3750,  0.2500},
   {-0.1875, -0.3125,  0.4375,  0.2500, -0.4375,  0.3125,  0.1875, -0.3750},
   { 0.4375,  0.1875, -0.3750,  0.3125,  0.2500, -0.4375, -0.1875,  0.3750},
   {-0.3125,  0.3750,  0.1875, -0.4375,  0.3750,  0.2500, -0.3125,  0.1875},
   { 0.2500, -0.4375,  0.3125,  0.1875, -0.3750,  0.3125,  0.4375, -0.2500},
   {-0.3750,  0.2500, -0.3125,  0.4375, -0.1875, -0.2500,  0.3750,  0.4375}},
  // Expert 1
  {{-0.3125,  0.4375, -0.1875,  0.3750,  0.2500, -0.4375,  0.3125, -0.2500},
   { 0.4375, -0.2500,  0.3125, -0.3750,  0.1875,  0.3125, -0.4375,  0.2500},
   {-0.2500,  0.3750,  0.4375, -0.1875, -0.3125,  0.2500,  0.3750, -0.4375},
   { 0.1875, -0.3125, -0.2500,  0.4375, -0.3750,  0.1875,  0.2500,  0.3125},
   {-0.4375,  0.1875, -0.3750,  0.2500,  0.3125, -0.3125,  0.1875,  0.4375},
   { 0.3750, -0.4375,  0.1875, -0.3125,  0.4375,  0.2500, -0.3750,  0.1875},
   {-0.2500,  0.3125,  0.4375, -0.3750,  0.1875, -0.4375,  0.3125, -0.2500},
   { 0.3125, -0.1875, -0.3750,  0.4375, -0.2500,  0.3750, -0.1875,  0.3125},
   {-0.4375,  0.2500,  0.1875, -0.3125,  0.3750, -0.2500,  0.4375, -0.3750},
   { 0.1875, -0.3750,  0.3125,  0.2500, -0.4375,  0.1875,  0.3750, -0.3125},
   {-0.3750,  0.4375, -0.2500,  0.3125,  0.1875, -0.3750,  0.2500,  0.4375},
   { 0.2500, -0.1875,  0.4375, -0.3750,  0.3125,  0.4375, -0.2500, -0.1875},
   {-0.1875,  0.3125, -0.3750,  0.2500,  0.4375, -0.3125,  0.1875,  0.3750},
   { 0.4375, -0.2500,  0.1875,  0.3750, -0.3125,  0.2500, -0.4375, -0.1875},
   {-0.3750,  0.1875,  0.3125, -0.4375,  0.2500,  0.3750, -0.3125,  0.4375},
   { 0.3125,  0.4375, -0.2500, -0.1875,  0.3750, -0.4375,  0.2500, -0.3750}},
  // Expert 2
  {{ 0.2500, -0.3125,  0.4375, -0.1875,  0.3750,  0.2500, -0.4375,  0.3125},
   {-0.4375,  0.1875, -0.2500,  0.3750, -0.3125,  0.4375,  0.1875, -0.3750},
   { 0.3750, -0.4375,  0.3125,  0.1875, -0.2500,  0.3125,  0.4375, -0.1875},
   {-0.1875,  0.2500,  0.3750, -0.4375,  0.1875, -0.3125,  0.2500,  0.4375},
   { 0.4375, -0.1875, -0.3125,  0.2500,  0.3750, -0.2500,  0.1875, -0.4375},
   {-0.3125,  0.4375,  0.1875, -0.3750,  0.2500,  0.1875, -0.3750,  0.3125},
   { 0.1875, -0.2500,  0.4375,  0.3125, -0.1875,  0.4375, -0.3125, -0.2500},
   {-0.2500,  0.3750, -0.1875, -0.3125,  0.4375, -0.3750,  0.3125,  0.1875},
   { 0.3125, -0.4375,  0.2500,  0.3750, -0.3125,  0.2500,  0.4375, -0.1875},
   {-0.3750,  0.1875,  0.3125, -0.2500,  0.4375, -0.1875, -0.3750,  0.2500},
   { 0.4375, -0.3750, -0.2500,  0.1875,  0.3125,  0.3750, -0.4375, -0.3125},
   {-0.2500,  0.3125,  0.4375, -0.3750,  0.1875, -0.2500,  0.3125,  0.4375},
   { 0.1875,  0.4375, -0.3125,  0.2500, -0.3750,  0.1875,  0.2500, -0.4375},
   {-0.4375,  0.2500,  0.1875,  0.3125,  0.4375, -0.3125, -0.1875,  0.3750},
   { 0.3750, -0.3125, -0.4375,  0.4375, -0.2500,  0.3750, -0.3125,  0.1875},
   {-0.1875, -0.2500,  0.3750,  0.4375, -0.4375,  0.2500,  0.3750, -0.3125}},
  // Expert 3
  {{-0.4375,  0.3125, -0.1875,  0.2500,  0.4375, -0.3750,  0.1875,  0.3125},
   { 0.1875, -0.4375,  0.3750, -0.3125,  0.2500,  0.4375, -0.2500, -0.1875},
   {-0.3750,  0.2500,  0.4375,  0.1875, -0.3125, -0.4375,  0.3750,  0.2500},
   { 0.2500, -0.1875, -0.3750,  0.4375,  0.1875,  0.3125, -0.4375,  0.3750},
   {-0.3125,  0.4375, -0.2500, -0.3750,  0.3125,  0.1875,  0.4375, -0.2500},
   { 0.4375, -0.3750,  0.1875,  0.3125, -0.4375,  0.2500, -0.1875,  0.3750},
   {-0.1875,  0.2500,  0.3750, -0.4375,  0.3750, -0.3125,  0.2500,  0.4375},
   { 0.3750, -0.3125, -0.4375,  0.2500,  0.1875,  0.4375, -0.3750, -0.2500},
   {-0.2500,  0.4375,  0.3125, -0.1875, -0.3750,  0.2500,  0.4375, -0.3125},
   { 0.4375, -0.2500, -0.1875,  0.3750, -0.3125, -0.4375,  0.1875,  0.3125},
   {-0.3125,  0.1875,  0.4375, -0.2500,  0.2500,  0.3750, -0.3125, -0.4375},
   { 0.1875,  0.3750, -0.3125,  0.1875, -0.4375,  0.3125,  0.2500,  0.4375},
   {-0.4375, -0.2500,  0.2500,  0.3125,  0.3750, -0.1875, -0.3750,  0.1875},
   { 0.3125,  0.4375, -0.4375, -0.3750,  0.1875,  0.2500,  0.3750, -0.3125},
   {-0.2500,  0.3125,  0.1875,  0.4375, -0.2500, -0.3125,  0.4375, -0.3750},
   { 0.4375, -0.1875,  0.3750, -0.3125,  0.3125,  0.4375, -0.2500, -0.1875}}
};

// Layer 2 weights: [N_EXPERTS][OUTPUT_DIM=1][HIDDEN_DIM]
static const fixed_t W2[4][1][16] = {
  {{ 0.3125, -0.2500,  0.4375, -0.1875,  0.3750,  0.2500, -0.4375,  0.3125,
    -0.2500,  0.1875,  0.3750, -0.3125,  0.4375, -0.2500,  0.1875, -0.4375}},
  {{-0.4375,  0.3750, -0.2500,  0.1875, -0.3125,  0.4375,  0.2500, -0.3750,
     0.1875, -0.4375,  0.3125,  0.2500, -0.1875,  0.3750, -0.3125,  0.4375}},
  {{ 0.1875, -0.3125,  0.2500,  0.4375, -0.3750,  0.1875,  0.3125, -0.4375,
     0.4375, -0.2500, -0.1875,  0.3750, -0.3125,  0.2500,  0.4375, -0.1875}},
  {{-0.3750,  0.2500,  0.1875, -0.4375,  0.3125,  0.3750, -0.2500,  0.1875,
    -0.3125,  0.4375, -0.3750,  0.1875,  0.2500, -0.4375,  0.3750,  0.3125}}
};

// Layer 1 biases: [N_EXPERTS][HIDDEN_DIM]
static const fixed_t B1[4][16] = {
  {0.0625,-0.0625, 0.0625,-0.0625, 0.0625,-0.0625, 0.0625,-0.0625,
   0.0625,-0.0625, 0.0625,-0.0625, 0.0625,-0.0625, 0.0625,-0.0625},
  {-0.0625,0.0625,-0.0625,0.0625,-0.0625,0.0625,-0.0625,0.0625,
   -0.0625,0.0625,-0.0625,0.0625,-0.0625,0.0625,-0.0625,0.0625},
  {0.0625, 0.0625,-0.0625,-0.0625,0.0625, 0.0625,-0.0625,-0.0625,
   0.0625, 0.0625,-0.0625,-0.0625,0.0625, 0.0625,-0.0625,-0.0625},
  {-0.0625,-0.0625,0.0625, 0.0625,-0.0625,-0.0625,0.0625, 0.0625,
   -0.0625,-0.0625,0.0625, 0.0625,-0.0625,-0.0625,0.0625, 0.0625}
};

// Layer 2 biases: [N_EXPERTS][1]
static const fixed_t B2[4][1] = {{0.0625}, {-0.0625}, {0.0625}, {-0.0625}};

// =========================================================================
// expert_forward — 2-layer MLP forward pass for one expert
//
// Layer 1: h[j] = ReLU(sum_i(W1[eid][j][i] * x[i]) + B1[eid][j])
// Layer 2: out  = sum_j(W2[eid][0][j] * h[j]) + B2[eid][0]
//
// With PIPELINE and UNROLL, HLS will implement this as a systolic array of
// multiply-accumulate cells, achieving II=1 per expert inference.
// =========================================================================
fixed_t expert_forward(const FeatureVector& feat, int expert_id) {
#pragma HLS INLINE

    const int eid = expert_id & 3; // clamp to [0,3]

    // Pack features into an array (required for loop-based unroll)
    fixed_t x[EXPERT_INPUT_DIM] = {
        feat.mid_price_norm,
        feat.spread_norm,
        feat.order_imbalance,
        feat.bid_qty_norm,
        feat.ask_qty_norm,
        feat.bid_ask_ratio,
        feat.spread_to_mid,
        feat.price_velocity
    };
#pragma HLS ARRAY_PARTITION variable=x complete

    // Layer 1: compute hidden activations with ReLU
    fixed_t h[EXPERT_HIDDEN_DIM];
#pragma HLS ARRAY_PARTITION variable=h complete

LAYER1:
    for (int j = 0; j < EXPERT_HIDDEN_DIM; ++j) {
#pragma HLS UNROLL
        fixed_t accum = B1[eid][j];
L1_DOT:
        for (int i = 0; i < EXPERT_INPUT_DIM; ++i) {
#pragma HLS UNROLL
            accum += W1[eid][j][i] * x[i];
        }
        // ReLU activation: free in hardware — just a comparator + MUX
        h[j] = (accum > 0.0) ? accum : 0.0;
    }

    // Layer 2: linear output (no activation on output layer)
    fixed_t out = B2[eid][0];
LAYER2:
    for (int j = 0; j < EXPERT_HIDDEN_DIM; ++j) {
#pragma HLS UNROLL
        out += W2[eid][0][j] * h[j];
    }

    return out;
}

// =========================================================================
// run_expert — top-level HLS function for one expert stream
// =========================================================================
void run_expert(
    hls::stream<RouterOutput>&  router_in,
    hls::stream<ExpertOutput>&  expert_out,
    int                         n_messages,
    ap_uint<4>                  expert_id
) {
#pragma HLS INTERFACE axis port=router_in   bundle=ROUTER_IN
#pragma HLS INTERFACE axis port=expert_out  bundle=EXPERT_OUT
#pragma HLS INTERFACE s_axilite port=n_messages bundle=CTRL
#pragma HLS INTERFACE s_axilite port=expert_id  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return     bundle=CTRL

EXPERT_LOOP:
    for (int i = 0; i < n_messages; ++i) {
#pragma HLS PIPELINE II=1

        RouterOutput rin = router_in.read();

        // Only run inference if this expert is one of the selected top-K
        bool selected = (rin.expert_id[0] == expert_id ||
                         rin.expert_id[1] == expert_id);

        ExpertOutput eout;
        eout.expert_id  = expert_id;
        eout.msg_type   = rin.msg_type;

        if (selected) {
            fixed_t raw = expert_forward(rin.features, static_cast<int>(expert_id));
            eout.raw_output = raw;
            // Retrieve gating weight for this expert
            eout.gate_weight = (rin.expert_id[0] == expert_id) ?
                                rin.gate_weight[0] : rin.gate_weight[1];
            // Apply threshold to produce discrete signal
            eout.trade_signal = (raw >  SIGNAL_THRESHOLD) ? 1 :  // BUY
                                 (raw < -SIGNAL_THRESHOLD) ? 2 :  // SELL
                                                              0;   // HOLD
        } else {
            // Expert not selected — zero contribution
            eout.raw_output  = 0.0;
            eout.gate_weight = 0.0;
            eout.trade_signal = 0;
        }

        expert_out.write(eout);
    }
}

// =========================================================================
// combine_experts — sum weighted outputs from the two active experts
// =========================================================================
void combine_experts(
    hls::stream<ExpertOutput>&  expert0_in,
    hls::stream<ExpertOutput>&  expert1_in,
    hls::stream<TradeDecision>& decision_out,
    int                         n_messages
) {
#pragma HLS INTERFACE axis port=expert0_in   bundle=EXPERT0_IN
#pragma HLS INTERFACE axis port=expert1_in   bundle=EXPERT1_IN
#pragma HLS INTERFACE axis port=decision_out bundle=DECISION_OUT
#pragma HLS INTERFACE s_axilite port=n_messages bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return     bundle=CTRL

COMBINE_LOOP:
    for (int i = 0; i < n_messages; ++i) {
#pragma HLS PIPELINE II=1

        ExpertOutput e0 = expert0_in.read();
        ExpertOutput e1 = expert1_in.read();

        // Weighted sum: final_output = w0 * out0 + w1 * out1
        fixed_t weighted = e0.gate_weight * e0.raw_output +
                            e1.gate_weight * e1.raw_output;

        TradeDecision d;
        d.weighted_output = weighted;
        d.msg_type        = e0.msg_type;
        d.trade_signal    = (weighted >  SIGNAL_THRESHOLD) ? 1 :
                             (weighted < -SIGNAL_THRESHOLD) ? 2 : 0;

        decision_out.write(d);
    }
}
