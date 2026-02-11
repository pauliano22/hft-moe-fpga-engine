// =============================================================================
// Expert MLP Kernel — Vitis HLS Implementation
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Implements one expert neural network as a fully-unrolled, pipelined
//   hardware module. When synthesized, this becomes a circuit that computes
//   a 2-layer MLP in a single clock cycle (thanks to full unrolling).
//
// HARDWARE RESOURCE USAGE (approximate per expert):
//   Layer 1: 16 neurons × 8 features = 128 multiplies → 128 DSP slices
//   Layer 2: 16 multiplies → 16 DSP slices
//   Total: ~144 DSP slices + associated LUTs and FFs
//
// SINCE WE HAVE 2 ACTIVE EXPERTS:
//   288 DSP slices total for expert inference — well within the budget
//   of modern FPGAs (UltraScale+ has 2,000-12,000 DSP slices).

#include "expert_mlp.h"

// =============================================================================
// ReLU Activation Function
// =============================================================================
// ReLU(x) = max(0, x)
//
// In software, this is a simple comparison. In hardware, it's even simpler:
// just check the sign bit. If the number is negative (sign bit = 1), output
// zero. Otherwise, pass through unchanged.
//
// This is one reason ReLU is so popular for hardware neural networks —
// it costs essentially zero logic resources.
static inline fixed_t relu(fixed_t x) {
    #pragma HLS INLINE  // Inline = no function call overhead in hardware
    return (x > 0) ? x : fixed_t(0);
}

// =============================================================================
// Expert MLP — Top-Level Function
// =============================================================================
void expert_mlp(
    hls::stream<ExpertInput>&  in,       // Features + gating weight from router
    hls::stream<ExpertOutput>& out,      // Scalar result + gating weight
    const ExpertWeights&       weights   // This expert's trained weights (BRAM)
) {
    // =========================================================================
    // HLS Interface Pragmas
    // =========================================================================
    #pragma HLS INTERFACE axis port=in    // AXI-Stream input
    #pragma HLS INTERFACE axis port=out   // AXI-Stream output
    #pragma HLS INTERFACE bram port=weights  // Weights stored in Block RAM

    // PIPELINE II=1: Process one input per clock cycle
    #pragma HLS PIPELINE II=1

    // ARRAY_PARTITION on weights: allow all elements to be read simultaneously.
    // Without this, we'd need multiple clock cycles to read all weights from BRAM.
    //
    // "complete dim=2" means: partition the second dimension (NUM_FEATURES)
    // completely, so each of the 8 features can be accessed in parallel.
    #pragma HLS ARRAY_PARTITION variable=weights.w1 complete dim=2
    #pragma HLS ARRAY_PARTITION variable=weights.b1 complete
    #pragma HLS ARRAY_PARTITION variable=weights.w2 complete

    // -------------------------------------------------------------------------
    // Read input from the router
    // -------------------------------------------------------------------------
    ExpertInput ei;
    if (!in.read_nb(ei)) return;  // Non-blocking read
    if (!ei.valid) return;

    // -------------------------------------------------------------------------
    // Layer 1: Input → Hidden (with ReLU activation)
    // -------------------------------------------------------------------------
    // For each hidden neuron h:
    //   hidden[h] = ReLU(bias1[h] + Σ(w1[h][f] × features[f]))
    //
    // With full UNROLL, all 16 neurons compute simultaneously.
    // Each neuron does 8 multiplies + 8 adds + 1 ReLU = single cycle.
    //
    // Hardware view:
    //   16 parallel MAC (multiply-accumulate) units, each with 8 inputs.
    //   That's 128 DSP slices running in parallel.
    fixed_t hidden[HIDDEN_DIM];
    #pragma HLS ARRAY_PARTITION variable=hidden complete  // All 16 values in registers

    LAYER1:
    for (int h = 0; h < HIDDEN_DIM; h++) {
        #pragma HLS UNROLL  // 16 parallel neurons
        acc_t sum = acc_t(weights.b1[h]);  // Start with bias

        LAYER1_INNER:
        for (int f = 0; f < NUM_FEATURES; f++) {
            #pragma HLS UNROLL  // 8 parallel multiply-adds per neuron
            sum += acc_t(weights.w1[h][f]) * acc_t(ei.features[f]);
        }

        // Apply ReLU: if sum < 0, output 0; otherwise pass through.
        // The fixed_t() cast truncates acc_t (32-bit) to fixed_t (16-bit).
        hidden[h] = relu(fixed_t(sum));
    }

    // -------------------------------------------------------------------------
    // Layer 2: Hidden → Output (scalar)
    // -------------------------------------------------------------------------
    // output = bias2 + Σ(w2[h] × hidden[h])
    //
    // This produces a single number: the expert's "opinion" about what
    // the market will do. Positive = bullish (buy), negative = bearish (sell).
    acc_t output_sum = acc_t(weights.b2);

    LAYER2:
    for (int h = 0; h < HIDDEN_DIM; h++) {
        #pragma HLS UNROLL  // 16 parallel multiplies
        output_sum += acc_t(weights.w2[h]) * acc_t(hidden[h]);
    }

    // -------------------------------------------------------------------------
    // Emit result
    // -------------------------------------------------------------------------
    // The expert output includes:
    //   - result: the scalar prediction
    //   - gate_weight: passed through from the router (for weighted combination)
    //   - valid: always true if we got here
    ExpertOutput eo;
    eo.result      = fixed_t(output_sum);  // Truncate back to 16-bit
    eo.gate_weight = ei.gate_weight;        // Pass through for final weighting
    eo.valid       = true;

    out.write(eo);  // Send to output stream
}
