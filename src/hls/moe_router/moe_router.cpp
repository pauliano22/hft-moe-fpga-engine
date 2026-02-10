// =============================================================================
// MoE Router — Vitis HLS Implementation
// =============================================================================

#include "moe_router.h"

// =============================================================================
// Top-K Selection (K=2) via Comparator Tree
// =============================================================================
static void top_k_select(
    acc_t scores[NUM_EXPERTS],
    int   top_idx[TOP_K],
    acc_t top_scores[TOP_K]
) {
    #pragma HLS INLINE

    // Initialize with worst-case values
    top_scores[0] = -128;
    top_scores[1] = -128;
    top_idx[0] = 0;
    top_idx[1] = 1;

    // Single pass to find top-2
    TOP_K_LOOP:
    for (int i = 0; i < NUM_EXPERTS; i++) {
        #pragma HLS UNROLL
        if (scores[i] > top_scores[0]) {
            // Demote current best to second
            top_scores[1] = top_scores[0];
            top_idx[1]    = top_idx[0];
            // New best
            top_scores[0] = scores[i];
            top_idx[0]    = i;
        } else if (scores[i] > top_scores[1]) {
            top_scores[1] = scores[i];
            top_idx[1]    = i;
        }
    }
}

// =============================================================================
// Softmax Approximation for 2 Values
// =============================================================================
// Uses the identity: softmax(a, b) = [1/(1+exp(b-a)), 1/(1+exp(a-b))]
// Approximated with a piecewise linear function for hardware efficiency.
static void softmax_approx(
    acc_t  score_0,
    acc_t  score_1,
    fixed_t& weight_0,
    fixed_t& weight_1
) {
    #pragma HLS INLINE
    #pragma HLS PIPELINE II=1

    acc_t diff = score_0 - score_1;

    // Piecewise linear approximation of sigmoid
    // sigmoid(x) ≈ 0.5 + 0.25*x for |x| < 2, clamped to [0,1]
    acc_t sigmoid_val;
    if (diff > 2) {
        sigmoid_val = 1;
    } else if (diff < -2) {
        sigmoid_val = 0;
    } else {
        sigmoid_val = acc_t(0.5) + acc_t(0.25) * diff;
    }

    weight_0 = fixed_t(sigmoid_val);
    weight_1 = fixed_t(acc_t(1) - sigmoid_val);
}

// =============================================================================
// MoE Router — Top-Level
// =============================================================================
void moe_router(
    hls::stream<FeatureVector>& features_in,
    hls::stream<ExpertInput>&   expert_out_0,
    hls::stream<ExpertInput>&   expert_out_1,
    const fixed_t weights[NUM_EXPERTS][NUM_FEATURES],
    const fixed_t biases[NUM_EXPERTS]
) {
    #pragma HLS INTERFACE axis port=features_in
    #pragma HLS INTERFACE axis port=expert_out_0
    #pragma HLS INTERFACE axis port=expert_out_1
    #pragma HLS INTERFACE bram port=weights
    #pragma HLS INTERFACE bram port=biases
    #pragma HLS PIPELINE II=1
    #pragma HLS ARRAY_PARTITION variable=weights complete dim=2

    // Read input
    FeatureVector fv;
    if (!features_in.read_nb(fv)) return;
    if (!fv.valid) return;

    // -------------------------------------------------------------------------
    // Step 1: Compute expert scores (linear projection)
    // -------------------------------------------------------------------------
    acc_t scores[NUM_EXPERTS];
    #pragma HLS ARRAY_PARTITION variable=scores complete

    SCORE_EXPERTS:
    for (int e = 0; e < NUM_EXPERTS; e++) {
        #pragma HLS UNROLL
        acc_t sum = acc_t(biases[e]);

        SCORE_FEATURES:
        for (int f = 0; f < NUM_FEATURES; f++) {
            #pragma HLS UNROLL
            sum += acc_t(weights[e][f]) * acc_t(fv.features[f]);
        }
        scores[e] = sum;
    }

    // -------------------------------------------------------------------------
    // Step 2: Top-K selection
    // -------------------------------------------------------------------------
    int   top_idx[TOP_K];
    acc_t top_scores[TOP_K];
    top_k_select(scores, top_idx, top_scores);

    // -------------------------------------------------------------------------
    // Step 3: Compute gating weights
    // -------------------------------------------------------------------------
    fixed_t gate_0, gate_1;
    softmax_approx(top_scores[0], top_scores[1], gate_0, gate_1);

    // -------------------------------------------------------------------------
    // Step 4: Dispatch to selected experts
    // -------------------------------------------------------------------------
    ExpertInput ei0, ei1;
    ei0.valid = true;
    ei1.valid = true;
    ei0.expert_id = top_idx[0];
    ei1.expert_id = top_idx[1];
    ei0.gate_weight = gate_0;
    ei1.gate_weight = gate_1;

    COPY_FEATURES:
    for (int f = 0; f < NUM_FEATURES; f++) {
        #pragma HLS UNROLL
        ei0.features[f] = fv.features[f];
        ei1.features[f] = fv.features[f];
    }

    expert_out_0.write(ei0);
    expert_out_1.write(ei1);
}
