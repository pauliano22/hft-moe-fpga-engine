// =============================================================================
// Expert MLP Kernel — Vitis HLS Implementation (Header)
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Defines the structure for a single expert neural network.
//   Each expert is a 2-layer MLP (Multi-Layer Perceptron) that takes
//   8 market features and produces a single scalar prediction.
//
// NEURAL NETWORK ARCHITECTURE:
//   Layer 1: 8 inputs → 16 hidden neurons (ReLU activation)
//   Layer 2: 16 hidden neurons → 1 output (linear activation)
//
//   In diagram form:
//     [f0 f1 f2 f3 f4 f5 f6 f7]    ← 8 input features
//              │ (× W1 + b1, ReLU)
//     [h0 h1 h2 ... h15]           ← 16 hidden neurons
//              │ (× W2 + b2)
//            [out]                   ← 1 scalar output
//
// WHY 2 LAYERS?
//   A 2-layer MLP can learn nonlinear patterns (thanks to ReLU) while
//   staying small enough to fit entirely in on-chip resources. Each expert
//   uses: 8×16 + 16 + 16×1 + 1 = 161 weight parameters.
//
// WHY SEPARATE EXPERTS?
//   Each expert specializes in a different market "regime":
//     - Expert 0 might learn trending patterns
//     - Expert 1 might learn mean-reversion patterns
//     - Expert 2 might learn high-volatility patterns
//     - etc.
//   The router decides which 2 experts are most relevant for the current input.

#ifndef EXPERT_MLP_H
#define EXPERT_MLP_H

#include "../moe_router/moe_router.h"  // Imports fixed_t, acc_t, ExpertInput, ExpertOutput

// Number of neurons in the hidden layer
// This is a balance between model capacity and hardware cost:
//   - More neurons = better learning, but more DSP slices
//   - 16 neurons × 8 features = 128 multiplies for layer 1
constexpr int HIDDEN_DIM = 16;

// All weights for one expert, stored in on-chip BRAM
struct ExpertWeights {
    fixed_t w1[HIDDEN_DIM][NUM_FEATURES];  // Layer 1: 16×8 weight matrix
    fixed_t b1[HIDDEN_DIM];                 // Layer 1: 16 bias values
    fixed_t w2[HIDDEN_DIM];                 // Layer 2: 16 weights (output is scalar)
    fixed_t b2;                              // Layer 2: 1 bias value
};

// Top-level expert function — synthesized into a hardware module
void expert_mlp(
    hls::stream<ExpertInput>&  in,       // Input: features + gating weight
    hls::stream<ExpertOutput>& out,      // Output: scalar result + gating weight
    const ExpertWeights&       weights   // BRAM: this expert's trained weights
);

#endif // EXPERT_MLP_H
