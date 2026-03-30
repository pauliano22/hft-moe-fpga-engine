#pragma once
// =========================================================================
// moe_router.hpp — HLS Sparse Mixture-of-Experts Router
//
// The MoE router takes the current order book state (from lob.cpp) and
// decides which of the N_EXPERTS specialist networks should process this
// market snapshot. The router implements:
//
//   1. Feature extraction: convert raw book integers to normalized
//      fixed-point inputs suitable for the gating network.
//
//   2. Gating network: a small linear layer (N_FEATURES → N_EXPERTS)
//      that computes a score for each expert. Implemented as a matrix-
//      vector multiply using ap_fixed<16,6> arithmetic (16 bits total,
//      6 integer bits, 10 fractional bits — resolution ~0.001).
//
//   3. Top-K selection: select the 2 experts with the highest scores.
//      Their gating weights are normalized so they sum to 1.0.
//
//   4. Output: expert indices + weights on hls::stream<RouterOutput>
//      for parallel dispatch to the expert kernels.
//
// Why Sparse MoE in hardware?
//   Dense MoE (all experts always active) would require N × M multiplies
//   per inference — expensive at sub-microsecond latency. Sparse MoE
//   activates only K=2 of N=4 experts per message, halving the compute.
//   On FPGA, the 2 experts run in parallel (II=1 each), so total latency
//   is the max latency of the 2 active experts, not their sum.
//
// Fixed-point choice (ap_fixed<16,6>):
//   6 integer bits → range [-32, 32) — sufficient for normalized features
//   10 fractional bits → resolution 1/1024 ≈ 0.001 — sufficient for
//   the precision needed in gating scores.
//   16 bits total → 2× the DSP efficiency of 32-bit float on Xilinx FPGAs.
// =========================================================================

#ifdef __SYNTHESIS__
  #include <ap_int.h>
  #include <ap_fixed.h>
  #include <hls_stream.h>
#else
  #include "../include/hls_sim_stubs.hpp"
#endif

#include "../matching_engine/lob.hpp"  // for BookUpdate

// -------------------------------------------------------------------------
// Router hyperparameters
// -------------------------------------------------------------------------
static const int N_FEATURES = 8;   // input feature vector dimension
static const int N_EXPERTS  = 4;   // total number of expert networks
static const int TOP_K      = 2;   // how many experts to activate per step

// Fixed-point type used throughout the MoE pipeline
using fixed_t = ap_fixed<16, 6>;

// -------------------------------------------------------------------------
// FeatureVector — the 8 normalized inputs to the gating network
//
// All features are normalized to approximately [-1, +1] before the gate.
// Normalization constants are determined from training data statistics.
// -------------------------------------------------------------------------
struct FeatureVector {
    fixed_t mid_price_norm;         // (mid_price - MID_NORM_MU) / MID_NORM_STD
    fixed_t spread_norm;            // spread / SPREAD_NORM_STD
    fixed_t order_imbalance;        // (bid_qty - ask_qty) / (bid_qty + ask_qty)
    fixed_t bid_qty_norm;           // bid_qty / QTY_NORM_STD
    fixed_t ask_qty_norm;           // ask_qty / QTY_NORM_STD
    fixed_t bid_ask_ratio;          // bid_qty / (ask_qty + 1)  — clamped to [-4, 4]
    fixed_t spread_to_mid;          // spread / mid_price
    fixed_t price_velocity;         // (mid_price - prev_mid) / prev_mid  (momentum)
};

// -------------------------------------------------------------------------
// RouterOutput — emitted per message to both expert kernel streams
// -------------------------------------------------------------------------
struct RouterOutput {
    ap_uint<4>  expert_id[TOP_K];   // indices of the two selected experts
    fixed_t     gate_weight[TOP_K]; // normalized gating weights (sum = 1.0)
    FeatureVector features;         // passed through to the selected experts
    ap_uint<8>  msg_type;           // passed through for downstream logic
};

// -------------------------------------------------------------------------
// Normalization constants (derived from one trading session of AAPL data)
// These would be loaded from AXI-Lite config registers in production.
// -------------------------------------------------------------------------
static const ap_uint<32> MID_NORM_MU  = 1800000; // $180.00 × 10000 center
static const ap_uint<32> MID_NORM_STD = 100000;  // $10.00  × 10000 std
static const ap_uint<32> SPREAD_STD   = 100;     // $0.01   × 10000 std
static const ap_uint<32> QTY_STD      = 10000;   // 10,000 shares std

// -------------------------------------------------------------------------
// Top-level HLS function
// -------------------------------------------------------------------------
void route_message(
    hls::stream<BookUpdate>&    book_in,
    hls::stream<RouterOutput>&  router_out,
    int                         n_messages
);

// Exposed for testbench
FeatureVector extract_features(const BookUpdate& b, ap_uint<32> prev_mid);
RouterOutput  gate_and_select (const FeatureVector& feat, ap_uint<8> msg_type);
