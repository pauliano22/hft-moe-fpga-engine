// =============================================================================
// Expert MLP Kernel — Vitis HLS Implementation
// =============================================================================
// Each expert is a small 2-layer MLP:
//   Layer 1: NUM_FEATURES → HIDDEN_DIM (ReLU activation)
//   Layer 2: HIDDEN_DIM → 1 (scalar output)
//
// All weights stored in on-chip BRAM. Each expert has its own weight set.

#ifndef EXPERT_MLP_H
#define EXPERT_MLP_H

#include "../moe_router/moe_router.h"

constexpr int HIDDEN_DIM = 16;

struct ExpertWeights {
    fixed_t w1[HIDDEN_DIM][NUM_FEATURES];  // Layer 1 weights
    fixed_t b1[HIDDEN_DIM];                 // Layer 1 biases
    fixed_t w2[HIDDEN_DIM];                 // Layer 2 weights (output is scalar)
    fixed_t b2;                              // Layer 2 bias
};

// Top-level expert function
void expert_mlp(
    hls::stream<ExpertInput>&  in,
    hls::stream<ExpertOutput>& out,
    const ExpertWeights&       weights
);

#endif // EXPERT_MLP_H
