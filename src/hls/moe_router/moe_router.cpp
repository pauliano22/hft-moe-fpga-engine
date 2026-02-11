// =============================================================================
// MoE Router — Vitis HLS Implementation
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Implements the Mixture of Experts router in synthesizable C++.
//   Vitis HLS compiles this into RTL (SystemVerilog) that can run on an FPGA.
//
// THE KEY INSIGHT:
//   This looks like normal C++ code, but the HLS pragmas transform it into
//   parallel hardware. For example, `#pragma HLS UNROLL` on a loop means
//   "don't loop sequentially — build 8 copies of the hardware and run them
//   all at the same time." This is spatial parallelism.
//
// ALGORITHM SUMMARY:
//   1. Read feature vector from input stream
//   2. Compute score for each expert: score[e] = bias[e] + Σ(w[e][f] × feat[f])
//   3. Find the top-2 experts by score
//   4. Compute gating weights using a piecewise-linear sigmoid approximation
//   5. Send features + weights to the two selected experts

#include "moe_router.h"

// =============================================================================
// Top-K Selection (K=2) via Comparator Tree
// =============================================================================
// Finds the 2 experts with the highest scores in a single pass.
//
// HARDWARE IMPLEMENTATION:
//   With #pragma HLS UNROLL, this becomes a tree of comparators:
//   - 8 comparisons happen simultaneously
//   - The result is the indices and scores of the top-2 experts
//   - Total latency: ~2 comparator delays (not 8 sequential comparisons)
//
// WHY NOT SORT?
//   Sorting all 8 scores would require O(N log N) comparisons.
//   We only need the top 2, so a single scan is O(N) and cheaper in hardware.
static void top_k_select(
    acc_t scores[NUM_EXPERTS],
    int   top_idx[TOP_K],
    acc_t top_scores[TOP_K]
) {
    #pragma HLS INLINE  // Merge this function into the caller (no function call overhead)

    // Initialize with worst-case values so any real score will be better
    top_scores[0] = -128;
    top_scores[1] = -128;
    top_idx[0] = 0;
    top_idx[1] = 1;

    // Single pass through all expert scores
    TOP_K_LOOP:
    for (int i = 0; i < NUM_EXPERTS; i++) {
        #pragma HLS UNROLL  // Build 8 parallel comparators, not a sequential loop
        if (scores[i] > top_scores[0]) {
            // New best: demote the old best to second place
            top_scores[1] = top_scores[0];
            top_idx[1]    = top_idx[0];
            // Install new best
            top_scores[0] = scores[i];
            top_idx[0]    = i;
        } else if (scores[i] > top_scores[1]) {
            // New second-best (but not best)
            top_scores[1] = scores[i];
            top_idx[1]    = i;
        }
    }
}

// =============================================================================
// Softmax Approximation for 2 Values
// =============================================================================
// Converts two raw scores into gating weights that sum to ~1.0.
//
// MATH BACKGROUND:
//   True softmax: softmax(a,b) = [exp(a)/(exp(a)+exp(b)), exp(b)/(exp(a)+exp(b))]
//   This simplifies to: [sigmoid(a-b), sigmoid(b-a)] = [σ(d), 1-σ(d)]
//   where d = a - b and σ(x) = 1/(1+exp(-x))
//
// HARDWARE APPROXIMATION:
//   exp() is expensive in hardware (requires CORDIC or lookup tables).
//   Instead, we use a piecewise linear approximation:
//     sigmoid(x) ≈ 0.5 + 0.25x    for |x| < 2
//     sigmoid(x) ≈ 1.0             for x > 2
//     sigmoid(x) ≈ 0.0             for x < -2
//
//   This uses only one multiply and one add — two DSP operations.
//   The maximum error vs. true sigmoid is ~0.07 at x=±1, which is
//   negligible for gating weights.
static void softmax_approx(
    acc_t  score_0,
    acc_t  score_1,
    fixed_t& weight_0,
    fixed_t& weight_1
) {
    #pragma HLS INLINE
    #pragma HLS PIPELINE II=1  // Can compute one softmax per clock cycle

    acc_t diff = score_0 - score_1;

    // Piecewise linear sigmoid approximation
    acc_t sigmoid_val;
    if (diff > 2) {
        sigmoid_val = 1;       // Expert 0 dominates completely
    } else if (diff < -2) {
        sigmoid_val = 0;       // Expert 1 dominates completely
    } else {
        // Linear region: sigmoid ≈ 0.5 + 0.25 × diff
        sigmoid_val = acc_t(0.5) + acc_t(0.25) * diff;
    }

    // Convert to output weights (must sum to ~1.0)
    weight_0 = fixed_t(sigmoid_val);
    weight_1 = fixed_t(acc_t(1) - sigmoid_val);
}

// =============================================================================
// MoE Router — Top-Level Function
// =============================================================================
// This function IS the hardware module. Vitis HLS synthesizes it into RTL.
// The pragmas control HOW the C++ maps to hardware circuits.
void moe_router(
    hls::stream<FeatureVector>& features_in,   // Input: market features
    hls::stream<ExpertInput>&   expert_out_0,   // Output: to expert #1
    hls::stream<ExpertInput>&   expert_out_1,   // Output: to expert #2
    const fixed_t weights[NUM_EXPERTS][NUM_FEATURES],  // Router weight matrix
    const fixed_t biases[NUM_EXPERTS]                   // Router bias vector
) {
    // =========================================================================
    // HLS Interface Pragmas — Define how C++ maps to hardware ports
    // =========================================================================
    #pragma HLS INTERFACE axis port=features_in   // AXI-Stream FIFO input
    #pragma HLS INTERFACE axis port=expert_out_0  // AXI-Stream FIFO output
    #pragma HLS INTERFACE axis port=expert_out_1  // AXI-Stream FIFO output
    #pragma HLS INTERFACE bram port=weights       // Store weights in Block RAM
    #pragma HLS INTERFACE bram port=biases        // Store biases in Block RAM

    // PIPELINE II=1: Accept one new feature vector every clock cycle.
    // "II" = Initiation Interval. II=1 is the best possible throughput.
    #pragma HLS PIPELINE II=1

    // ARRAY_PARTITION: Split the weights array so all features can be
    // read simultaneously. Without this, BRAM only allows 2 reads/cycle,
    // and we need 8 (one per feature for each expert).
    #pragma HLS ARRAY_PARTITION variable=weights complete dim=2

    // -------------------------------------------------------------------------
    // Read input feature vector
    // -------------------------------------------------------------------------
    FeatureVector fv;
    if (!features_in.read_nb(fv)) return;  // Non-blocking read (no stall if empty)
    if (!fv.valid) return;                  // Skip invalid data

    // -------------------------------------------------------------------------
    // Step 1: Compute expert scores (linear projection)
    // -------------------------------------------------------------------------
    // For each expert e: score[e] = bias[e] + Σ(weights[e][f] × features[f])
    //
    // With UNROLL, all 8 experts compute their scores simultaneously.
    // Each expert's score computation is also fully unrolled (8 multiplies
    // + 8 adds), so this entire step takes ONE clock cycle.
    //
    // In hardware, this uses 8 × 8 = 64 DSP slices (one per multiply).
    acc_t scores[NUM_EXPERTS];
    #pragma HLS ARRAY_PARTITION variable=scores complete  // All 8 scores in registers

    SCORE_EXPERTS:
    for (int e = 0; e < NUM_EXPERTS; e++) {
        #pragma HLS UNROLL  // 8 parallel score computations
        acc_t sum = acc_t(biases[e]);

        SCORE_FEATURES:
        for (int f = 0; f < NUM_FEATURES; f++) {
            #pragma HLS UNROLL  // 8 parallel multiply-adds per expert
            sum += acc_t(weights[e][f]) * acc_t(fv.features[f]);
        }
        scores[e] = sum;
    }

    // -------------------------------------------------------------------------
    // Step 2: Top-K selection — find the 2 best experts
    // -------------------------------------------------------------------------
    int   top_idx[TOP_K];
    acc_t top_scores[TOP_K];
    top_k_select(scores, top_idx, top_scores);

    // -------------------------------------------------------------------------
    // Step 3: Compute gating weights (how much to trust each expert)
    // -------------------------------------------------------------------------
    fixed_t gate_0, gate_1;
    softmax_approx(top_scores[0], top_scores[1], gate_0, gate_1);

    // -------------------------------------------------------------------------
    // Step 4: Dispatch features to the selected experts
    // -------------------------------------------------------------------------
    // Each expert receives: the original features + its gating weight + its ID.
    // The two experts run in parallel on separate hardware.
    ExpertInput ei0, ei1;
    ei0.valid = true;
    ei1.valid = true;
    ei0.expert_id = top_idx[0];
    ei1.expert_id = top_idx[1];
    ei0.gate_weight = gate_0;
    ei1.gate_weight = gate_1;

    // Copy features to both experts
    COPY_FEATURES:
    for (int f = 0; f < NUM_FEATURES; f++) {
        #pragma HLS UNROLL  // Parallel copy, not sequential
        ei0.features[f] = fv.features[f];
        ei1.features[f] = fv.features[f];
    }

    // Write to output streams (these become AXI-Stream transactions)
    expert_out_0.write(ei0);
    expert_out_1.write(ei1);
}
