// =============================================================================
// MoE Router — Vitis HLS Implementation
// =============================================================================
// The router takes a feature vector and selects the top-K experts.
// Uses fixed-point arithmetic for deterministic, low-latency inference.
//
// Architecture:
//   1. Linear projection: features × weight_matrix → expert_scores
//   2. Top-K selection: find the K highest scores via comparator tree
//   3. Softmax approximation: normalize selected scores
//   4. Output: expert indices + gating weights

#ifndef MOE_ROUTER_H
#define MOE_ROUTER_H

#include <ap_fixed.h>
#include <hls_stream.h>
#include <ap_int.h>

// =============================================================================
// Type Definitions
// =============================================================================

// Fixed-point: 16 bits total, 8 integer, 8 fractional
// Range: [-128, +127.99609375], Resolution: ~0.004
typedef ap_fixed<16, 8> fixed_t;

// Higher precision for intermediate computations
typedef ap_fixed<32, 16> acc_t;

// =============================================================================
// Configuration Parameters
// =============================================================================
constexpr int NUM_FEATURES   = 8;    // Input feature dimension
constexpr int NUM_EXPERTS    = 8;    // Total number of experts
constexpr int TOP_K          = 2;    // Number of active experts per input

// =============================================================================
// Data Structures
// =============================================================================

struct FeatureVector {
    fixed_t features[NUM_FEATURES];
    bool    valid;
};

struct RouterOutput {
    int     expert_idx[TOP_K];        // Selected expert indices
    fixed_t gate_weight[TOP_K];       // Gating weights (sum to ~1.0)
    bool    valid;
};

struct ExpertInput {
    fixed_t features[NUM_FEATURES];
    fixed_t gate_weight;               // This expert's gating weight
    int     expert_id;
    bool    valid;
};

struct ExpertOutput {
    fixed_t result;                    // Scalar output from expert
    fixed_t gate_weight;               // Pass through for weighted sum
    bool    valid;
};

struct TradeSignal {
    ap_uint<2>  action;                // 0=Hold, 1=Buy, 2=Sell
    fixed_t     confidence;
    ap_uint<32> price;
    ap_uint<32> quantity;
    bool        valid;
};

// =============================================================================
// Top-Level Function Declaration
// =============================================================================

void moe_router(
    hls::stream<FeatureVector>& features_in,
    hls::stream<ExpertInput>&   expert_out_0,
    hls::stream<ExpertInput>&   expert_out_1,
    // Router weights (loaded once, then static)
    const fixed_t weights[NUM_EXPERTS][NUM_FEATURES],
    const fixed_t biases[NUM_EXPERTS]
);

#endif // MOE_ROUTER_H
