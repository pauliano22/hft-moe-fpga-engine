// =========================================================================
// moe_router.cpp — HLS MoE Router implementation
// =========================================================================

#ifdef __SYNTHESIS__
  #include <ap_int.h>
  #include <ap_fixed.h>
  #include <hls_stream.h>
#else
  #include "../include/hls_sim_stubs.hpp"
#endif

#include "moe_router.hpp"

// =========================================================================
// Gating network weights — W[N_EXPERTS][N_FEATURES] + bias[N_EXPERTS]
//
// These are randomly initialized compile-time constants (demo purposes).
// In a deployed system, weights would be trained on labeled market data
// (e.g., supervised on "next-tick price direction") and loaded via an
// AXI-Lite register interface from a host CPU after FPGA configuration.
//
// Fixed-point ap_fixed<16,6> synthesis:
//   Each weight is a 16-bit constant → synthesized to LUT logic (no BRAM).
//   A 4×8 weight matrix = 32 multiplications per gating step, each a
//   fixed-point DSP48 multiply → ~32 DSPs. At 250 MHz, the full gating
//   network completes in ~3 clock cycles with full pipelining.
// =========================================================================
static const fixed_t GATE_W[N_EXPERTS][N_FEATURES] = {
    { 0.3125,  -0.1875,  0.5000, -0.2500,  0.1250,  0.3750, -0.0625,  0.4375 },
    {-0.2500,   0.4375, -0.3125,  0.5000, -0.1875,  0.2500,  0.3125, -0.4375 },
    { 0.4375,  -0.3750,  0.1875,  0.3125, -0.5000,  0.4375, -0.2500,  0.3750 },
    {-0.1875,   0.2500,  0.4375, -0.3125,  0.3750, -0.4375,  0.5000, -0.3125 }
};

static const fixed_t GATE_BIAS[N_EXPERTS] = {
    0.0625, -0.0625, 0.0625, -0.0625
};

// =========================================================================
// extract_features — convert raw BookUpdate integers to normalized features
// =========================================================================
FeatureVector extract_features(const BookUpdate& b, ap_uint<32> prev_mid) {
#pragma HLS INLINE

    FeatureVector f;

    // Mid-price normalization: (mid - 180.00) / 10.00, in × 10000 units
    // Result in [-18, 18] for typical price range; clamped implicitly by
    // fixed_t range of [-32, 32)
    if (b.mid_price > 0) {
        fixed_t raw_mid = static_cast<fixed_t>(
            static_cast<int64_t>(b.mid_price) -
            static_cast<int64_t>(MID_NORM_MU)) /
            static_cast<fixed_t>(MID_NORM_STD);
        // Clamp to fixed_t range
        f.mid_price_norm = (raw_mid >  16.0) ?  16.0 :
                           (raw_mid < -16.0) ? -16.0 : raw_mid;
    } else {
        f.mid_price_norm = 0.0;
    }

    // Spread normalization: spread / 0.01 (spread expressed in ticks)
    f.spread_norm = (b.spread > 0) ?
        static_cast<fixed_t>(b.spread) / static_cast<fixed_t>(SPREAD_STD) : 0.0;

    // Order imbalance ratio: (bid_qty - ask_qty) / (bid_qty + ask_qty)
    ap_uint<32> total_qty = b.bid_total_qty + b.ask_total_qty;
    if (total_qty > 0) {
        fixed_t bid_f = static_cast<fixed_t>(b.bid_total_qty);
        fixed_t ask_f = static_cast<fixed_t>(b.ask_total_qty);
        fixed_t tot_f = static_cast<fixed_t>(total_qty);
        f.order_imbalance = (bid_f - ask_f) / tot_f;
    } else {
        f.order_imbalance = 0.0;
    }

    // Bid/ask quantity normalization: qty / 10,000 shares
    f.bid_qty_norm = static_cast<fixed_t>(b.bid_total_qty) /
                     static_cast<fixed_t>(QTY_STD);
    f.ask_qty_norm = static_cast<fixed_t>(b.ask_total_qty) /
                     static_cast<fixed_t>(QTY_STD);

    // Bid/ask ratio: clamp to [-4, 4] to prevent blowup
    if (b.ask_total_qty > 0) {
        fixed_t ratio = static_cast<fixed_t>(b.bid_total_qty) /
                        static_cast<fixed_t>(b.ask_total_qty);
        f.bid_ask_ratio = (ratio > 4.0) ? 4.0 : (ratio < -4.0) ? -4.0 : ratio;
    } else {
        f.bid_ask_ratio = (b.bid_total_qty > 0) ? 4.0 : 0.0;
    }

    // Spread-to-mid: spread / mid_price (proxy for relative cost of crossing)
    if (b.mid_price > 0 && b.spread > 0) {
        f.spread_to_mid = static_cast<fixed_t>(b.spread) /
                          static_cast<fixed_t>(b.mid_price);
    } else {
        f.spread_to_mid = 0.0;
    }

    // Price velocity: (mid - prev_mid) / prev_mid (momentum signal)
    if (prev_mid > 0 && b.mid_price > 0) {
        int64_t delta = static_cast<int64_t>(b.mid_price) -
                        static_cast<int64_t>(prev_mid);
        f.price_velocity = static_cast<fixed_t>(delta) /
                           static_cast<fixed_t>(prev_mid);
    } else {
        f.price_velocity = 0.0;
    }

    return f;
}

// =========================================================================
// gate_and_select — compute gating scores and select top-2 experts
//
// Gating: score[k] = GATE_W[k] · features + GATE_BIAS[k]
//         This is a dot product — synthesizes to 8 parallel DSP48 multiplies
//         followed by an adder tree.
//
// Top-K: scan the 4 scores, select the 2 largest indices.
//         With N_EXPERTS=4, this is 6 comparisons — trivial in hardware.
//
// Weight normalization: normalize the top-2 scores using softmax approx:
//         w[i] = exp(score[i]) / sum(exp(score[top-2]))
//         We use a linear approximation for softmax to avoid exp():
//         w[i] = score[i] / (score[0] + score[1])  after ensuring > 0.
// =========================================================================
RouterOutput gate_and_select(const FeatureVector& feat, ap_uint<8> msg_type) {
#pragma HLS INLINE

    // Pack features into an array for matrix multiply
    fixed_t fv[N_FEATURES] = {
        feat.mid_price_norm,
        feat.spread_norm,
        feat.order_imbalance,
        feat.bid_qty_norm,
        feat.ask_qty_norm,
        feat.bid_ask_ratio,
        feat.spread_to_mid,
        feat.price_velocity
    };
#pragma HLS ARRAY_PARTITION variable=fv complete

    // Compute gating scores: score[k] = sum_j(W[k][j] * fv[j]) + bias[k]
    fixed_t score[N_EXPERTS];
#pragma HLS ARRAY_PARTITION variable=score complete

GATE_COMPUTE:
    for (int k = 0; k < N_EXPERTS; ++k) {
#pragma HLS UNROLL
        fixed_t accum = GATE_BIAS[k];
DOT_PRODUCT:
        for (int j = 0; j < N_FEATURES; ++j) {
#pragma HLS UNROLL
            accum += GATE_W[k][j] * fv[j];
        }
        score[k] = accum;
    }

    // Top-2 selection: find the two highest scoring experts.
    // With N_EXPERTS=4, this is a simple compare-and-swap network.
    ap_uint<4> top0_idx = 0, top1_idx = 1;
    fixed_t    top0_val = score[0], top1_val = score[1];

    // Ensure top0 >= top1
    if (top1_val > top0_val) {
        ap_uint<4> ti = top0_idx; top0_idx = top1_idx; top1_idx = ti;
        fixed_t    tv = top0_val; top0_val = top1_val; top1_val = tv;
    }

    // Compare remaining experts against the current top-2
TOP_K_SELECT:
    for (int k = 2; k < N_EXPERTS; ++k) {
#pragma HLS UNROLL
        if (score[k] > top0_val) {
            top1_idx = top0_idx; top1_val = top0_val;
            top0_idx = k;        top0_val = score[k];
        } else if (score[k] > top1_val) {
            top1_idx = k;        top1_val = score[k];
        }
    }

    // Normalize gating weights.
    // Shift scores to be positive before normalizing: add max(|min_score|, 0).
    fixed_t min_val = (top0_val < top1_val) ? top0_val : top1_val;
    fixed_t shift   = (min_val < 0.0) ? -min_val + 0.0625 : 0.0625; // +epsilon
    fixed_t s0 = top0_val + shift;
    fixed_t s1 = top1_val + shift;
    fixed_t sum = s0 + s1;

    RouterOutput out;
    out.expert_id[0]    = top0_idx;
    out.expert_id[1]    = top1_idx;
    out.gate_weight[0]  = (sum > 0.0) ? s0 / sum : 0.5;
    out.gate_weight[1]  = (sum > 0.0) ? s1 / sum : 0.5;
    out.features        = feat;
    out.msg_type        = msg_type;
    return out;
}

// =========================================================================
// route_message — top-level HLS function
// =========================================================================
void route_message(
    hls::stream<BookUpdate>&   book_in,
    hls::stream<RouterOutput>& router_out,
    int                        n_messages
) {
#pragma HLS INTERFACE axis port=book_in    bundle=BOOK_IN
#pragma HLS INTERFACE axis port=router_out bundle=ROUTER_OUT
#pragma HLS INTERFACE s_axilite port=n_messages bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return     bundle=CTRL

    // Maintain previous mid price for velocity calculation.
    // Static = persists across invocations (across messages in the stream).
    static ap_uint<32> prev_mid = 0;

ROUTER_LOOP:
    for (int i = 0; i < n_messages; ++i) {
#pragma HLS PIPELINE II=1

        BookUpdate b = book_in.read();

        FeatureVector feat = extract_features(b, prev_mid);
        RouterOutput  out  = gate_and_select(feat, b.msg_type);

        // Update previous mid for next iteration (data-flow dependency OK for II=1)
        if (b.mid_price > 0) prev_mid = b.mid_price;

        router_out.write(out);
    }
}
