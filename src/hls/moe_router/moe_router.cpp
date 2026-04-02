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

static const fixed_t GATE_W[N_EXPERTS][N_FEATURES] = {
    { 0.3125f, -0.1875f, 0.50f, -0.2500f, 0.1250f, 0.3750f, -0.0625f, 0.4375f },
    {-0.2500f,  0.4375f, -0.3125f, 0.50f, -0.1875f, 0.2500f,  0.3125f, -0.4375f },
    { 0.4375f, -0.3750f, 0.1875f, 0.3125f, -0.50f,  0.4375f, -0.2500f,  0.3750f },
    {-0.1875f,  0.2500f,  0.4375f, -0.3125f, 0.3750f, -0.4375f,  0.50f, -0.3125f }
};

static const fixed_t GATE_BIAS[N_EXPERTS] = {
    0.0625f, -0.0625f, 0.0625f, -0.0625f
};

FeatureVector extract_features(const BookUpdate& b, ap_uint<32> prev_mid) {
#pragma HLS INLINE
    FeatureVector f;

    if (b.mid_price > 0) {
        fixed_t diff = static_cast<fixed_t>(static_cast<int64_t>(b.mid_price) - static_cast<int64_t>(MID_NORM_MU));
        fixed_t raw_mid = static_cast<fixed_t>(diff / static_cast<fixed_t>(MID_NORM_STD));

        if (raw_mid > (fixed_t)16.0f) f.mid_price_norm = (fixed_t)16.0f;
        else if (raw_mid < (fixed_t)-16.0f) f.mid_price_norm = (fixed_t)-16.0f;
        else f.mid_price_norm = raw_mid;
    } else {
        f.mid_price_norm = (fixed_t)0;
    }

    if (b.spread > 0) {
        f.spread_norm = static_cast<fixed_t>(static_cast<fixed_t>(b.spread) / static_cast<fixed_t>(SPREAD_STD));
    } else {
        f.spread_norm = (fixed_t)0;
    }

    ap_uint<32> total_qty = b.bid_total_qty + b.ask_total_qty;
    if (total_qty > 0) {
        fixed_t diff_q = static_cast<fixed_t>(static_cast<int32_t>(b.bid_total_qty) - static_cast<int32_t>(b.ask_total_qty));
        f.order_imbalance = static_cast<fixed_t>(diff_q / static_cast<fixed_t>(total_qty));
    } else {
        f.order_imbalance = (fixed_t)0;
    }

    f.bid_qty_norm = static_cast<fixed_t>(static_cast<fixed_t>(b.bid_total_qty) / static_cast<fixed_t>(QTY_STD));
    f.ask_qty_norm = static_cast<fixed_t>(static_cast<fixed_t>(b.ask_total_qty) / static_cast<fixed_t>(QTY_STD));

    if (b.ask_total_qty > 0) {
        fixed_t ratio = static_cast<fixed_t>(static_cast<fixed_t>(b.bid_total_qty) / static_cast<fixed_t>(b.ask_total_qty));
        if (ratio > (fixed_t)4.0f) f.bid_ask_ratio = (fixed_t)4.0f;
        else if (ratio < (fixed_t)-4.0f) f.bid_ask_ratio = (fixed_t)-4.0f;
        else f.bid_ask_ratio = ratio;
    } else {
        f.bid_ask_ratio = (b.bid_total_qty > 0) ? (fixed_t)4.0f : (fixed_t)0;
    }

    if (b.mid_price > 0 && b.spread > 0) {
        f.spread_to_mid = static_cast<fixed_t>(static_cast<fixed_t>(b.spread) / static_cast<fixed_t>(b.mid_price));
    } else {
        f.spread_to_mid = (fixed_t)0;
    }

    if (prev_mid > 0 && b.mid_price > 0) {
        fixed_t delta = static_cast<fixed_t>(static_cast<int64_t>(b.mid_price) - static_cast<int64_t>(prev_mid));
        f.price_velocity = static_cast<fixed_t>(delta / static_cast<fixed_t>(prev_mid));
    } else {
        f.price_velocity = (fixed_t)0;
    }

    return f;
}

RouterOutput gate_and_select(const FeatureVector& feat, ap_uint<8> msg_type) {
#pragma HLS INLINE
    fixed_t fv[N_FEATURES] = {
        feat.mid_price_norm, feat.spread_norm, feat.order_imbalance,
        feat.bid_qty_norm, feat.ask_qty_norm, feat.bid_ask_ratio,
        feat.spread_to_mid, feat.price_velocity
    };
#pragma HLS ARRAY_PARTITION variable=fv complete

    fixed_t score[N_EXPERTS];
#pragma HLS ARRAY_PARTITION variable=score complete

GATE_COMPUTE:
    for (int k = 0; k < N_EXPERTS; ++k) {
#pragma HLS UNROLL
        fixed_t accum = GATE_BIAS[k];
DOT_PRODUCT:
        for (int j = 0; j < N_FEATURES; ++j) {
#pragma HLS UNROLL
            accum += static_cast<fixed_t>(GATE_W[k][j] * fv[j]);
        }
        score[k] = accum;
    }

    ap_uint<4> top0_idx = 0, top1_idx = 1;
    fixed_t top0_val = score[0], top1_val = score[1];

    if (top1_val > top0_val) {
        ap_uint<4> ti = top0_idx; top0_idx = top1_idx; top1_idx = ti;
        fixed_t tv = top0_val; top0_val = top1_val; top1_val = tv;
    }

TOP_K_SELECT:
    for (int k = 2; k < N_EXPERTS; ++k) {
#pragma HLS UNROLL
        if (score[k] > top0_val) {
            top1_idx = top0_idx; top1_val = top0_val;
            top0_idx = k; top0_val = score[k];
        } else if (score[k] > top1_val) {
            top1_idx = k; top1_val = score[k];
        }
    }

    fixed_t min_v = (top0_val < top1_val) ? top0_val : top1_val;
    fixed_t shift;
    if (min_v < (fixed_t)0) {
        shift = static_cast<fixed_t>(static_cast<fixed_t>(-min_v) + (fixed_t)0.0625f);
    } else {
        shift = (fixed_t)0.0625f;
    }

    fixed_t s0 = static_cast<fixed_t>(top0_val + shift);
    fixed_t s1 = static_cast<fixed_t>(top1_val + shift);
    fixed_t sum_s = static_cast<fixed_t>(s0 + s1);

    RouterOutput out;
    out.expert_id[0] = top0_idx;
    out.expert_id[1] = top1_idx;
    
    if (sum_s > (fixed_t)0) {
        out.gate_weight[0] = static_cast<fixed_t>(s0 / sum_s);
        out.gate_weight[1] = static_cast<fixed_t>(s1 / sum_s);
    } else {
        out.gate_weight[0] = (fixed_t)0.5f;
        out.gate_weight[1] = (fixed_t)0.5f;
    }
    
    out.features = feat;
    out.msg_type = msg_type;
    return out;
}

void route_message(hls::stream<BookUpdate>& book_in, hls::stream<RouterOutput>& router_out, int n_messages) {
#pragma HLS INTERFACE axis port=book_in
#pragma HLS INTERFACE axis port=router_out
#pragma HLS INTERFACE s_axilite port=n_messages bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    static ap_uint<32> prev_mid_reg = 0;

ROUTER_LOOP:
    for (int i = 0; i < n_messages; ++i) {
#pragma HLS PIPELINE II=1
        BookUpdate b = book_in.read();
        FeatureVector feat = extract_features(b, prev_mid_reg);
        RouterOutput out = gate_and_select(feat, b.msg_type);
        if (b.mid_price > 0) prev_mid_reg = b.mid_price;
        router_out.write(out);
    }
}