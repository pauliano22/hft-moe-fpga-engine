#pragma once
// =========================================================================
// expert_kernel.hpp — HLS MLP Expert Kernel
//
// Each "expert" is a small 2-layer MLP that produces a single trade signal
// (positive = buy, negative = sell, ~0 = hold) given the feature vector
// from the MoE router.
//
// Architecture:
//   Layer 1: input(8) → hidden(16) with ReLU activation
//   Layer 2: hidden(16) → output(1) — raw logit, no activation
//
// Why this architecture in hardware?
//   - 8→16→1 fits in ~128 DSP48 multiplies (8×16 + 16×1 = 144).
//     On xcvu9p: 6840 DSPs available → 47 expert instances could run in
//     parallel. We instantiate 2 for TOP_K=2 dispatch.
//   - II=1 on the forward pass with PIPELINE and UNROLL pragmas.
//   - ReLU is free in hardware: a comparator + MUX, no DSP.
//   - All weights are compile-time constants → synthesized to ROM (LUTs
//     or BRAM depending on size) for instant access.
//
// Trade signal interpretation:
//   output >  SIGNAL_THRESHOLD → BUY  (signal = +1)
//   output < -SIGNAL_THRESHOLD → SELL (signal = -1)
//   otherwise                  → HOLD (signal =  0)
// =========================================================================

#ifdef __SYNTHESIS__
  #include <ap_int.h>
  #include <ap_fixed.h>
  #include <hls_stream.h>
#else
  #include "../include/hls_sim_stubs.hpp"
#endif

#include "../moe_router/moe_router.hpp"  // for RouterOutput, FeatureVector, fixed_t

// -------------------------------------------------------------------------
// MLP dimensions
// -------------------------------------------------------------------------
static const int EXPERT_INPUT_DIM  = N_FEATURES;  // 8
static const int EXPERT_HIDDEN_DIM = 16;
static const int EXPERT_OUTPUT_DIM = 1;

// Decision threshold: |output| > this → non-hold signal
static const double SIGNAL_THRESHOLD = 0.1;

// -------------------------------------------------------------------------
// ExpertOutput — emitted by each expert kernel
// -------------------------------------------------------------------------
struct ExpertOutput {
    fixed_t     raw_output;    // raw MLP output before thresholding
    fixed_t     gate_weight;   // gating weight from router (for weighted sum)
    ap_uint<2>  trade_signal;  // 0=HOLD, 1=BUY, 2=SELL (after threshold)
    ap_uint<4>  expert_id;     // which expert produced this output
    ap_uint<8>  msg_type;      // passed through from router
};

// -------------------------------------------------------------------------
// TradeDecision — final output combining both active experts
// -------------------------------------------------------------------------
struct TradeDecision {
    fixed_t    weighted_output; // w0 * out0 + w1 * out1
    ap_uint<2> trade_signal;    // 0=HOLD, 1=BUY, 2=SELL
    ap_uint<8> msg_type;
};

// -------------------------------------------------------------------------
// Top-level HLS functions
// -------------------------------------------------------------------------

// Process one expert (called for each of the TOP_K active experts)
void run_expert(
    hls::stream<RouterOutput>&  router_in,
    hls::stream<ExpertOutput>&  expert_out,
    int                         n_messages,
    ap_uint<4>                  expert_id   // selects which weight set to use
);

// Combine two expert outputs into a final trade decision
void combine_experts(
    hls::stream<ExpertOutput>&   expert0_in,
    hls::stream<ExpertOutput>&   expert1_in,
    hls::stream<TradeDecision>&  decision_out,
    int                          n_messages
);

// Exposed for testbench
fixed_t expert_forward(const FeatureVector& feat, int expert_id);
