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

// Weights and Biases with explicit float literals
static const fixed_t W1[4][16][8] = {
  // Expert 0
  {{ 0.3125f,  0.1875f, -0.4375f,  0.2500f,  0.3750f, -0.1250f,  0.4375f, -0.3125f},
   {-0.2500f,  0.4375f,  0.1875f, -0.3125f,  0.2500f,  0.4375f, -0.3750f,  0.1250f},
   { 0.4375f, -0.3125f,  0.2500f,  0.3750f, -0.1875f, -0.4375f,  0.3125f,  0.2500f},
   {-0.1875f,  0.2500f, -0.4375f,  0.1875f,  0.3125f, -0.2500f,  0.4375f, -0.3750f},
   { 0.3750f, -0.1875f,  0.3125f, -0.4375f,  0.1875f,  0.3750f, -0.2500f,  0.4375f},
   {-0.4375f,  0.3125f,  0.2500f, -0.1875f,  0.4375f, -0.3750f,  0.1875f, -0.3125f},
   { 0.1875f, -0.4375f,  0.3750f,  0.2500f, -0.3125f,  0.1875f,  0.4375f, -0.2500f},
   {-0.3750f,  0.2500f, -0.1875f,  0.4375f,  0.2500f, -0.3125f,  0.1875f,  0.3125f},
   { 0.2500f,  0.3125f, -0.3750f, -0.2500f,  0.4375f,  0.1875f, -0.4375f,  0.3750f},
   {-0.4375f, -0.1875f,  0.3125f,  0.3750f, -0.2500f,  0.4375f,  0.2500f, -0.1875f},
   { 0.3125f,  0.4375f, -0.2500f, -0.3125f,  0.1875f, -0.3750f,  0.3750f,  0.2500f},
   {-0.1875f, -0.3125f,  0.4375f,  0.2500f, -0.4375f,  0.3125f,  0.1875f, -0.3750f},
   { 0.4375f,  0.1875f, -0.3750f,  0.3125f,  0.2500f, -0.4375f, -0.1875f,  0.3750f},
   {-0.3125f,  0.3750f,  0.1875f, -0.4375f,  0.3750f,  0.2500f, -0.3125f,  0.1875f},
   { 0.2500f, -0.4375f,  0.3125f,  0.1875f, -0.3750f,  0.3125f,  0.4375f, -0.2500f},
   {-0.3750f,  0.2500f, -0.3125f,  0.4375f, -0.1875f, -0.2500f,  0.3750f,  0.4375f}},
  // Expert 1
  {{-0.3125f,  0.4375f, -0.1875f,  0.3750f,  0.2500f, -0.4375f,  0.3125f, -0.2500f},
   { 0.4375f, -0.2500f,  0.3125f, -0.3750f,  0.1875f,  0.3125f, -0.4375f,  0.2500f},
   {-0.2500f,  0.3750f,  0.4375f, -0.1875f, -0.3125f,  0.2500f,  0.3750f, -0.4375f},
   { 0.1875f, -0.3125f, -0.2500f,  0.4375f, -0.3750f,  0.1875f,  0.2500f,  0.3125f},
   {-0.4375f,  0.1875f, -0.3750f,  0.2500f,  0.3125f, -0.3125f,  0.1875f,  0.4375f},
   { 0.3750f, -0.4375f,  0.1875f, -0.3125f,  0.4375f,  0.2500f, -0.3750f,  0.1875f},
   {-0.2500f,  0.3125f,  0.4375f, -0.3750f,  0.1875f, -0.4375f,  0.3125f, -0.2500f},
   { 0.3125f, -0.1875f, -0.3750f,  0.4375f, -0.2500f,  0.3750f, -0.1875f,  0.3125f},
   {-0.4375f,  0.2500f,  0.1875f, -0.3125f,  0.3750f, -0.2500f,  0.4375f, -0.3750f},
   { 0.1875f, -0.3750f,  0.3125f,  0.2500f, -0.4375f,  0.1875f,  0.3750f, -0.3125f},
   {-0.3750f,  0.4375f, -0.2500f,  0.3125f,  0.1875f, -0.3750f,  0.2500f,  0.4375f},
   { 0.2500f, -0.1875f,  0.4375f, -0.3750f,  0.3125f,  0.4375f, -0.2500f, -0.1875f},
   {-0.1875f,  0.3125f, -0.3750f,  0.2500f,  0.4375f, -0.3125f,  0.1875f,  0.3750f},
   { 0.4375f, -0.2500f,  0.1875f,  0.3750f, -0.3125f,  0.2500f, -0.4375f, -0.1875f},
   {-0.3750f,  0.1875f,  0.3125f, -0.4375f,  0.2500f,  0.3750f, -0.3125f,  0.4375f},
   { 0.3125f,  0.4375f, -0.2500f, -0.1875f,  0.3750f, -0.4375f,  0.2500f, -0.3750f}},
  // Expert 2
  {{ 0.2500f, -0.3125f,  0.4375f, -0.1875f,  0.3750f,  0.2500f, -0.4375f,  0.3125f},
   {-0.4375f,  0.1875f, -0.2500f,  0.3750f, -0.3125f,  0.4375f,  0.1875f, -0.3750f},
   { 0.3750f, -0.4375f,  0.3125f,  0.1875f, -0.2500f,  0.3125f,  0.4375f, -0.1875f},
   {-0.1875f,  0.2500f,  0.3750f, -0.4375f,  0.1875f, -0.3125f,  0.2500f,  0.4375f},
   { 0.4375f, -0.1875f, -0.3125f,  0.2500f,  0.3750f, -0.2500f,  0.1875f, -0.4375f},
   {-0.3125f,  0.4375f,  0.1875f, -0.3750f,  0.2500f,  0.1875f, -0.3750f,  0.3125f},
   { 0.1875f, -0.2500f,  0.4375f,  0.3125f, -0.1875f,  0.4375f, -0.3125f, -0.2500f},
   {-0.2500f,  0.3750f, -0.1875f, -0.3125f,  0.4375f, -0.3750f,  0.3125f,  0.1875f},
   { 0.3125f, -0.4375f,  0.2500f,  0.3750f, -0.3125f,  0.2500f,  0.4375f, -0.1875f},
   {-0.3750f,  0.1875f,  0.3125f, -0.2500f,  0.4375f, -0.1875f, -0.3750f,  0.2500f},
   { 0.4375f, -0.3750f, -0.2500f,  0.1875f,  0.3125f,  0.3750f, -0.4375f, -0.3125f},
   {-0.2500f,  0.3125f,  0.4375f, -0.3750f,  0.1875f, -0.2500f,  0.3125f,  0.4375f},
   { 0.1875f,  0.4375f, -0.3125f,  0.2500f, -0.3750f,  0.1875f,  0.2500f, -0.4375f},
   {-0.4375f,  0.2500f,  0.1875f,  0.3125f,  0.4375f, -0.3125f, -0.1875f,  0.3750f},
   { 0.3750f, -0.3125f, -0.4375f,  0.4375f, -0.2500f,  0.3750f, -0.3125f,  0.1875f},
   {-0.1875f, -0.2500f,  0.3750f,  0.4375f, -0.4375f,  0.2500f,  0.3750f, -0.3125f}},
  // Expert 3
  {{-0.4375f,  0.3125f, -0.1875f,  0.2500f,  0.4375f, -0.3750f,  0.1875f,  0.3125f},
   { 0.1875f, -0.4375f,  0.3750f, -0.3125f,  0.2500f,  0.4375f, -0.2500f, -0.1875f},
   {-0.3750f,  0.2500f,  0.4375f,  0.1875f, -0.3125f, -0.4375f,  0.3750f,  0.2500f},
   { 0.2500f, -0.1875f, -0.3750f,  0.4375f,  0.1875f,  0.3125f, -0.4375f,  0.3750f},
   {-0.3125f,  0.4375f, -0.2500f, -0.3750f,  0.3125f,  0.1875f,  0.4375f, -0.2500f},
   { 0.4375f, -0.3750f,  0.1875f,  0.3125f, -0.4375f,  0.2500f, -0.1875f,  0.3750f},
   {-0.1875f,  0.2500f,  0.3750f, -0.4375f,  0.3750f, -0.3125f,  0.2500f,  0.4375f},
   { 0.3750f, -0.3125f, -0.4375f,  0.2500f,  0.1875f,  0.4375f, -0.3750f, -0.2500f},
   {-0.2500f,  0.4375f,  0.3125f, -0.1875f, -0.3750f,  0.2500f,  0.4375f, -0.3125f},
   { 0.4375f, -0.2500f, -0.1875f,  0.3750f, -0.3125f, -0.4375f,  0.1875f,  0.3125f},
   {-0.3125f,  0.1875f,  0.4375f, -0.2500f,  0.2500f,  0.3750f, -0.3125f, -0.4375f},
   { 0.1875f,  0.3750f, -0.3125f,  0.1875f, -0.4375f,  0.3125f,  0.2500f,  0.4375f},
   {-0.4375f, -0.2500f,  0.2500f,  0.3125f,  0.3750f, -0.1875f, -0.3750f,  0.1875f},
   { 0.3125f,  0.4375f, -0.4375f, -0.3750f,  0.1875f,  0.2500f,  0.3750f, -0.3125f},
   {-0.2500f,  0.3125f,  0.1875f,  0.4375f, -0.2500f, -0.3125f,  0.4375f, -0.3750f},
   { 0.4375f, -0.1875f,  0.3750f, -0.3125f,  0.3125f,  0.4375f, -0.2500f, -0.1875f}}
};

static const fixed_t W2[4][1][16] = {
  {{ 0.3125f, -0.2500f,  0.4375f, -0.1875f,  0.3750f,  0.2500f, -0.4375f,  0.3125f,
    -0.2500f,  0.1875f,  0.3750f, -0.3125f,  0.4375f, -0.2500f,  0.1875f, -0.4375f}},
  {{-0.4375f,  0.3750f, -0.2500f,  0.1875f, -0.3125f,  0.4375f,  0.2500f, -0.3750f,
     0.1875f, -0.4375f,  0.3125f,  0.2500f, -0.1875f,  0.3750f, -0.3125f,  0.4375f}},
  {{ 0.1875f, -0.3125f,  0.2500f,  0.4375f, -0.3750f,  0.1875f,  0.3125f, -0.4375f,
     0.4375f, -0.2500f, -0.1875f,  0.3750f, -0.3125f,  0.2500f,  0.4375f, -0.1875f}},
  {{-0.3750f,  0.2500f,  0.1875f, -0.4375f,  0.3125f,  0.3750f, -0.2500f,  0.1875f,
    -0.3125f,  0.4375f, -0.3750f,  0.1875f,  0.2500f, -0.4375f,  0.3750f,  0.3125f}}
};

static const fixed_t B1[4][16] = {
  {0.0625f,-0.0625f, 0.0625f,-0.0625f, 0.0625f,-0.0625f, 0.0625f,-0.0625f,
   0.0625f,-0.0625f, 0.0625f,-0.0625f, 0.0625f,-0.0625f, 0.0625f,-0.0625f},
  {-0.0625f,0.0625f,-0.0625f,0.0625f,-0.0625f,0.0625f,-0.0625f,0.0625f,
   -0.0625f,0.0625f,-0.0625f,0.0625f,-0.0625f,0.0625f,-0.0625f,0.0625f},
  {0.0625f, 0.0625f,-0.0625f,-0.0625f,0.0625f, 0.0625f,-0.0625f,-0.0625f,
   0.0625f, 0.0625f,-0.0625f,-0.0625f,0.0625f, 0.0625f,-0.0625f,-0.0625f},
  {-0.0625f,-0.0625f,0.0625f, 0.0625f,-0.0625f,-0.0625f,0.0625f, 0.0625f,
   -0.0625f,-0.0625f,0.0625f, 0.0625f,-0.0625f,-0.0625f,0.0625f, 0.0625f}
};

static const fixed_t B2[4][1] = {{0.0625f}, {-0.0625f}, {0.0625f}, {-0.0625f}};

fixed_t expert_forward(const FeatureVector& feat, int expert_id) {
#pragma HLS INLINE

    const int eid = expert_id & 3;

    fixed_t x[EXPERT_INPUT_DIM] = {
        feat.mid_price_norm, feat.spread_norm, feat.order_imbalance,
        feat.bid_qty_norm, feat.ask_qty_norm, feat.bid_ask_ratio,
        feat.spread_to_mid, feat.price_velocity
    };
#pragma HLS ARRAY_PARTITION variable=x complete

    fixed_t h[EXPERT_HIDDEN_DIM];
#pragma HLS ARRAY_PARTITION variable=h complete

LAYER1:
    for (int j = 0; j < EXPERT_HIDDEN_DIM; ++j) {
#pragma HLS UNROLL
        fixed_t accum = B1[eid][j];
L1_DOT:
        for (int i = 0; i < EXPERT_INPUT_DIM; ++i) {
#pragma HLS UNROLL
            accum += static_cast<fixed_t>(W1[eid][j][i] * x[i]);
        }
        // ReLU Fixed: use (fixed_t)0 to avoid double ambiguity
        h[j] = (accum > (fixed_t)0) ? accum : (fixed_t)0;
    }

    fixed_t out = B2[eid][0];
LAYER2:
    for (int j = 0; j < EXPERT_HIDDEN_DIM; ++j) {
#pragma HLS UNROLL
        out += static_cast<fixed_t>(W2[eid][0][j] * h[j]);
    }

    return out;
}

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
        bool selected = (rin.expert_id[0] == expert_id || rin.expert_id[1] == expert_id);

        ExpertOutput eout;
        eout.expert_id  = expert_id;
        eout.msg_type   = rin.msg_type;

        if (selected) {
            fixed_t raw = expert_forward(rin.features, static_cast<int>(expert_id));
            eout.raw_output = raw;
            eout.gate_weight = (rin.expert_id[0] == expert_id) ? rin.gate_weight[0] : rin.gate_weight[1];
            
            // Fixed thresholds
            if (raw > (fixed_t)SIGNAL_THRESHOLD) eout.trade_signal = 1;
            else if (raw < (fixed_t)-SIGNAL_THRESHOLD) eout.trade_signal = 2;
            else eout.trade_signal = 0;
        } else {
            eout.raw_output  = (fixed_t)0;
            eout.gate_weight = (fixed_t)0;
            eout.trade_signal = 0;
        }

        expert_out.write(eout);
    }
}

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

        fixed_t weighted = static_cast<fixed_t>(e0.gate_weight * e0.raw_output) +
                           static_cast<fixed_t>(e1.gate_weight * e1.raw_output);

        TradeDecision d;
        d.weighted_output = weighted;
        d.msg_type        = e0.msg_type;
        
        if (weighted > (fixed_t)SIGNAL_THRESHOLD) d.trade_signal = 1;
        else if (weighted < (fixed_t)-SIGNAL_THRESHOLD) d.trade_signal = 2;
        else d.trade_signal = 0;

        decision_out.write(d);
    }
}