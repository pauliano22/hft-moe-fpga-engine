// =============================================================================
// Expert MLP Kernel — Implementation
// =============================================================================

#include "expert_mlp.h"

// ReLU activation (hardware-friendly: just check sign bit)
static inline fixed_t relu(fixed_t x) {
    #pragma HLS INLINE
    return (x > 0) ? x : fixed_t(0);
}

void expert_mlp(
    hls::stream<ExpertInput>&  in,
    hls::stream<ExpertOutput>& out,
    const ExpertWeights&       weights
) {
    #pragma HLS INTERFACE axis port=in
    #pragma HLS INTERFACE axis port=out
    #pragma HLS INTERFACE bram port=weights
    #pragma HLS PIPELINE II=1
    #pragma HLS ARRAY_PARTITION variable=weights.w1 complete dim=2
    #pragma HLS ARRAY_PARTITION variable=weights.b1 complete
    #pragma HLS ARRAY_PARTITION variable=weights.w2 complete

    ExpertInput ei;
    if (!in.read_nb(ei)) return;
    if (!ei.valid) return;

    // -------------------------------------------------------------------------
    // Layer 1: Input → Hidden (with ReLU)
    // -------------------------------------------------------------------------
    fixed_t hidden[HIDDEN_DIM];
    #pragma HLS ARRAY_PARTITION variable=hidden complete

    LAYER1:
    for (int h = 0; h < HIDDEN_DIM; h++) {
        #pragma HLS UNROLL
        acc_t sum = acc_t(weights.b1[h]);

        LAYER1_INNER:
        for (int f = 0; f < NUM_FEATURES; f++) {
            #pragma HLS UNROLL
            sum += acc_t(weights.w1[h][f]) * acc_t(ei.features[f]);
        }
        hidden[h] = relu(fixed_t(sum));
    }

    // -------------------------------------------------------------------------
    // Layer 2: Hidden → Output (scalar)
    // -------------------------------------------------------------------------
    acc_t output_sum = acc_t(weights.b2);

    LAYER2:
    for (int h = 0; h < HIDDEN_DIM; h++) {
        #pragma HLS UNROLL
        output_sum += acc_t(weights.w2[h]) * acc_t(hidden[h]);
    }

    // -------------------------------------------------------------------------
    // Emit result
    // -------------------------------------------------------------------------
    ExpertOutput eo;
    eo.result      = fixed_t(output_sum);
    eo.gate_weight = ei.gate_weight;
    eo.valid       = true;

    out.write(eo);
}
