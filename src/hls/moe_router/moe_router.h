// =============================================================================
// MoE Router — Vitis HLS Implementation (Header)
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Defines the types and function signature for the Mixture of Experts router.
//   The router is the "brain's receptionist" — it looks at market data features
//   and decides which 2 (of 8) expert neural networks should analyze this input.
//
// HOW THE ROUTER WORKS:
//   1. Takes a feature vector (8 numbers describing market state)
//   2. Multiplies features by a weight matrix to get a "score" per expert
//   3. Picks the top-2 highest-scoring experts (Top-K selection)
//   4. Normalizes the scores into "gating weights" (how much to trust each)
//   5. Dispatches the features to the selected experts
//
// WHY TOP-K (SPARSE) INSTEAD OF ALL EXPERTS?
//   Running all 8 experts would take 8× more compute. By only running 2,
//   we keep the latency budget under 500ns while still having 8 experts
//   worth of model capacity. Different market conditions naturally route
//   to different expert pairs.
//
// HLS-SPECIFIC CONCEPTS:
//   - ap_fixed<16,8>: Fixed-point type. 16 total bits, 8 integer, 8 fractional.
//     This replaces floating-point for deterministic, fast hardware math.
//   - hls::stream<T>: FIFO interface that maps to AXI-Stream in hardware.
//     Data is pushed/popped one item at a time, like a queue.
//   - The function signature here becomes the hardware module's port list
//     when Vitis HLS synthesizes it into RTL.

#ifndef MOE_ROUTER_H
#define MOE_ROUTER_H

#include <ap_fixed.h>       // Xilinx fixed-point types
#include <hls_stream.h>     // Xilinx streaming FIFO interface
#include <ap_int.h>         // Xilinx arbitrary-width integers

// =============================================================================
// Type Definitions
// =============================================================================

// Fixed-point: 16 bits total, 8 integer, 8 fractional
// Range: [-128, +127.99609375]
// Resolution: 1/256 ≈ 0.00390625
//
// Why these sizes?
//   - 8 integer bits: Covers the range needed for normalized market features
//   - 8 fractional bits: ~0.4% precision, sufficient for trading signals
//   - 16 bits total: Fits in one DSP slice on Xilinx UltraScale+
typedef ap_fixed<16, 8> fixed_t;

// Higher precision for intermediate multiply-accumulate computations.
// When we multiply two 16-bit numbers, the result needs 32 bits to avoid
// overflow. We use this for running sums, then truncate back to fixed_t
// for the final result.
typedef ap_fixed<32, 16> acc_t;

// =============================================================================
// Configuration Parameters
// =============================================================================
constexpr int NUM_FEATURES   = 8;    // 8 input features per market event
constexpr int NUM_EXPERTS    = 8;    // 8 specialized expert networks
constexpr int TOP_K          = 2;    // Only the best 2 experts run per input

// =============================================================================
// Data Structures
// =============================================================================

// Input to the router: 8 fixed-point features + a valid flag
struct FeatureVector {
    fixed_t features[NUM_FEATURES];  // Market microstructure signals
    bool    valid;                   // True when data is ready to process
};

// Router's output: which experts were selected and their gating weights
struct RouterOutput {
    int     expert_idx[TOP_K];       // Indices of selected experts (0-7)
    fixed_t gate_weight[TOP_K];      // Gating weights (sum to ~1.0)
    bool    valid;
};

// What gets sent to each selected expert
struct ExpertInput {
    fixed_t features[NUM_FEATURES];  // Same features that entered the router
    fixed_t gate_weight;             // How much this expert's output matters
    int     expert_id;               // Which expert this is (for weight selection)
    bool    valid;
};

// What comes back from each expert
struct ExpertOutput {
    fixed_t result;                  // Scalar prediction from this expert
    fixed_t gate_weight;             // Passed through for weighted combination
    bool    valid;
};

// Final output: the trading decision
struct TradeSignal {
    ap_uint<2>  action;              // 0=Hold (do nothing), 1=Buy, 2=Sell
    fixed_t     confidence;          // How sure the model is (0.0 to ~1.0)
    ap_uint<32> price;               // Suggested limit price
    ap_uint<32> quantity;            // Suggested order size
    bool        valid;
};

// =============================================================================
// Top-Level Function Declaration
// =============================================================================
// When Vitis HLS synthesizes this function, it becomes a hardware module:
//   - features_in  → AXI-Stream slave port (input FIFO)
//   - expert_out_0 → AXI-Stream master port (output FIFO to expert 0)
//   - expert_out_1 → AXI-Stream master port (output FIFO to expert 1)
//   - weights      → BRAM port (on-chip memory storing the weight matrix)
//   - biases       → BRAM port (on-chip memory storing bias values)

void moe_router(
    hls::stream<FeatureVector>& features_in,
    hls::stream<ExpertInput>&   expert_out_0,
    hls::stream<ExpertInput>&   expert_out_1,
    const fixed_t weights[NUM_EXPERTS][NUM_FEATURES],
    const fixed_t biases[NUM_EXPERTS]
);

#endif // MOE_ROUTER_H
