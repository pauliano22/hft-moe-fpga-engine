#!/usr/bin/env python3
"""
Train a Sparse Mixture of Experts model on synthetic market data.
Exports quantized weights to SystemVerilog and C++ header files.

Usage:
    python3 tools/train_moe.py

Outputs:
    src/rtl/moe_weights.svh   — SystemVerilog localparam arrays
    src/golden_model/weights.h — C++ constexpr int16_t arrays
"""

import numpy as np
import os
import sys

# =============================================================================
# Configuration
# =============================================================================
NUM_FEATURES = 8
NUM_EXPERTS = 8
TOP_K = 2
HIDDEN_DIM = 16
FRAC_BITS = 8
SCALE = 1 << FRAC_BITS  # 256

NUM_SAMPLES = 10000
BATCH_SIZE = 64
LEARNING_RATE = 0.003
NUM_EPOCHS = 200
SEED = 42


# =============================================================================
# Hardware-matching feature extraction (for inference)
# =============================================================================
def highest_bit(val):
    """Find position of highest set bit (matches RTL priority encoder)."""
    val = int(val) & 0xFFFFFFFF
    if val == 0:
        return 0
    for i in range(31, -1, -1):
        if val & (1 << i):
            return i
    return 0


def arith_rshift(val, shift):
    """Arithmetic right shift matching RTL >>> operator."""
    val = int(val)
    if val >= 0:
        return val >> shift
    else:
        return -(-val >> shift)


def extract_features_hw_raw(price, shares, side_is_buy, best_bid, best_ask):
    """Extract features using hardware-identical shift-based arithmetic.
    Returns raw int16 values (ap_fixed<16,8> format)."""
    raw = [0] * NUM_FEATURES
    price = int(price)
    shares = int(shares)
    best_bid = int(best_bid)
    best_ask = int(best_ask)

    # Feature 0: Normalized price (shift-based division)
    mid = (best_bid + best_ask) >> 1
    if mid > 0:
        delta = price - mid
        log2_mid = highest_bit(mid)
        shift = max(0, log2_mid - 8)
        raw[0] = max(-32768, min(32767, arith_rshift(delta, shift)))

    # Feature 1: Side indicator (+256 buy, -256 sell = +/-1.0)
    raw[1] = 256 if side_is_buy else -256

    # Feature 2: Log2(quantity) / 16, in fixed-point = log2 << 4
    log2_qty = highest_bit(max(1, shares))
    raw[2] = log2_qty << 4

    # Feature 3: Spread normalized: (spread * 13) >> 9
    spread = (best_ask - best_bid) if best_ask > best_bid else 0
    raw[3] = max(-32768, min(32767, (spread * 13) >> 9))

    # Feature 4: Distance from best (shift-based)
    if side_is_buy:
        dist = best_bid - price if best_bid > 0 else 0
    else:
        dist = price - best_ask if best_ask > 0 else 0
    if mid > 0:
        log2_mid = highest_bit(mid)
        shift = max(0, log2_mid - 8)
        raw[4] = max(-32768, min(32767, arith_rshift(dist, shift)))

    return raw


# =============================================================================
# Float feature extraction (for training — better dynamic range)
# =============================================================================
def extract_features_float(price, shares, side_is_buy, best_bid, best_ask):
    """Extract float features for training (same semantics as HW but precise)."""
    features = np.zeros(NUM_FEATURES)
    price = float(price)
    best_bid = float(best_bid)
    best_ask = float(best_ask)
    shares = float(shares)

    mid = (best_bid + best_ask) / 2.0
    if mid > 0:
        features[0] = (price - mid) / mid
    features[1] = 1.0 if side_is_buy else -1.0
    features[2] = np.log2(max(1.0, shares)) / 16.0
    spread = max(0.0, best_ask - best_bid)
    features[3] = spread / 10000.0
    if side_is_buy:
        features[4] = (best_bid - price) / best_bid if best_bid > 0 else 0.0
    else:
        features[4] = (price - best_ask) / best_ask if best_ask > 0 else 0.0
    return features


# =============================================================================
# Synthetic data generation
# =============================================================================
def generate_synthetic_data(n_samples, seed=42):
    """Generate synthetic market data with learnable feature-label correlations.
    Features have good dynamic range and labels depend on features
    (with noise) so the MoE can learn a meaningful mapping."""
    rng = np.random.RandomState(seed)

    X = np.zeros((n_samples, NUM_FEATURES))

    # Generate diverse features with good range
    X[:, 0] = rng.randn(n_samples) * 0.5       # normalized price deviation
    X[:, 1] = rng.choice([-1.0, 1.0], n_samples)  # side indicator
    X[:, 2] = rng.uniform(0.3, 0.7, n_samples)  # log qty (normalized)
    X[:, 3] = rng.exponential(0.03, n_samples)   # spread
    X[:, 4] = rng.randn(n_samples) * 0.3        # distance from best
    # Features 5-7 remain zero (reserved)

    # Ground truth: nonlinear function the MoE should learn
    # Different "regimes" where different experts should specialize:
    # - Momentum: positive price + buy side → buy
    # - Mean reversion: extreme price + opposite side → counter-trade
    # - Spread: wide spread + distance → hold
    signal = (0.6 * X[:, 0] +
              0.3 * X[:, 1] +
              0.4 * X[:, 0] * X[:, 1] +     # interaction term
              0.2 * (X[:, 2] - 0.5) +        # quantity effect
              -0.5 * np.abs(X[:, 3]) * 10 +  # wide spread → hold
              0.3 * X[:, 4])
    noise = rng.randn(n_samples) * 0.15
    y = np.clip(signal + noise, -1.0, 1.0)

    return X, y


# =============================================================================
# Adam optimizer
# =============================================================================
class AdamOptimizer:
    def __init__(self, lr=0.001, beta1=0.9, beta2=0.999, eps=1e-8):
        self.lr = lr
        self.beta1 = beta1
        self.beta2 = beta2
        self.eps = eps
        self.m = {}
        self.v = {}
        self.t = 0

    def step(self, params, grads):
        self.t += 1
        for key in params:
            if key not in grads:
                continue
            if key not in self.m:
                self.m[key] = np.zeros_like(params[key])
                self.v[key] = np.zeros_like(params[key])

            g = np.clip(grads[key], -5.0, 5.0)  # gradient clipping
            self.m[key] = self.beta1 * self.m[key] + (1 - self.beta1) * g
            self.v[key] = self.beta2 * self.v[key] + (1 - self.beta2) * g ** 2

            m_hat = self.m[key] / (1 - self.beta1 ** self.t)
            v_hat = self.v[key] / (1 - self.beta2 ** self.t)

            params[key] -= self.lr * m_hat / (np.sqrt(v_hat) + self.eps)


def softmax(x):
    e = np.exp(x - x.max(axis=-1, keepdims=True))
    return e / e.sum(axis=-1, keepdims=True)


# =============================================================================
# MoE Training
# =============================================================================
def train_moe(X, y, num_epochs=200, batch_size=64, lr=0.003, seed=42):
    rng = np.random.RandomState(seed)
    N = X.shape[0]

    # Initialize weights
    params = {
        'router_w': rng.randn(NUM_EXPERTS, NUM_FEATURES) * 0.5,
        'router_b': np.zeros(NUM_EXPERTS),
    }
    # Diverse expert initialization — different biases so router gets gradient signal
    for e in range(NUM_EXPERTS):
        params[f'w1_{e}'] = rng.randn(HIDDEN_DIM, NUM_FEATURES) * np.sqrt(2.0 / NUM_FEATURES)
        params[f'b1_{e}'] = rng.randn(HIDDEN_DIM) * 0.1
        params[f'w2_{e}'] = rng.randn(HIDDEN_DIM) * np.sqrt(2.0 / HIDDEN_DIM)
        # Key: diverse output biases so experts start with different predictions
        params[f'b2_{e}'] = np.array([0.8 * ((e % 3) - 1)])  # -0.8, 0, 0.8, ...

    optimizer = AdamOptimizer(lr=lr)

    for epoch in range(num_epochs):
        perm = rng.permutation(N)
        X_shuf = X[perm]
        y_shuf = y[perm]

        epoch_loss = 0.0
        n_batches = 0

        for start in range(0, N, batch_size):
            end = min(start + batch_size, N)
            Xb = X_shuf[start:end]
            yb = y_shuf[start:end]
            bs = Xb.shape[0]

            # Forward
            scores = Xb @ params['router_w'].T + params['router_b']
            gates = softmax(scores)

            expert_outs = np.zeros((bs, NUM_EXPERTS))
            hidden_cache = {}
            pre_relu_cache = {}

            for e in range(NUM_EXPERTS):
                pre_relu = Xb @ params[f'w1_{e}'].T + params[f'b1_{e}']
                hidden = np.maximum(pre_relu, 0)
                out = hidden @ params[f'w2_{e}'] + params[f'b2_{e}']
                expert_outs[:, e] = out
                hidden_cache[e] = hidden
                pre_relu_cache[e] = pre_relu

            combined = (gates * expert_outs).sum(axis=1)

            loss = ((combined - yb) ** 2).mean()
            epoch_loss += loss
            n_batches += 1

            # Backward
            d_combined = 2.0 * (combined - yb) / bs

            d_gates = d_combined[:, None] * expert_outs
            d_expert_outs = d_combined[:, None] * gates

            inner = (d_gates * gates).sum(axis=1, keepdims=True)
            d_scores = gates * (d_gates - inner)

            grads = {}
            grads['router_w'] = d_scores.T @ Xb
            grads['router_b'] = d_scores.sum(axis=0)

            for e in range(NUM_EXPERTS):
                d_out = d_expert_outs[:, e]
                grads[f'b2_{e}'] = np.array([d_out.sum()])
                grads[f'w2_{e}'] = hidden_cache[e].T @ d_out

                d_hidden = d_out[:, None] * params[f'w2_{e}'][None, :]
                d_pre_relu = d_hidden * (pre_relu_cache[e] > 0).astype(float)
                grads[f'b1_{e}'] = d_pre_relu.sum(axis=0)
                grads[f'w1_{e}'] = d_pre_relu.T @ Xb

            optimizer.step(params, grads)

        if (epoch + 1) % 20 == 0:
            print(f"  Epoch {epoch + 1:3d}/{num_epochs} — Loss: {epoch_loss / n_batches:.6f}")

    return params


# =============================================================================
# Quantization
# =============================================================================
def quantize(val):
    """Quantize float to ap_fixed<16,8> raw int16."""
    raw = int(round(float(val) * SCALE))
    return max(-32768, min(32767, raw))


# =============================================================================
# Evaluate model with top-2 gating (matches hardware inference)
# =============================================================================
def evaluate_top2(X, params):
    """Run MoE inference with top-2 expert selection (matching HW)."""
    N = X.shape[0]
    predictions = np.zeros(N)

    for i in range(N):
        x = X[i]
        # Router scores
        scores = params['router_w'] @ x + params['router_b']

        # Top-2 selection
        top2 = np.argsort(scores)[-2:][::-1]

        # Piecewise linear sigmoid softmax (matching HW)
        diff = scores[top2[0]] - scores[top2[1]]
        if diff > 2.0:
            sigmoid = 1.0
        elif diff < -2.0:
            sigmoid = 0.0
        else:
            sigmoid = 0.5 + 0.25 * diff
        gate_0 = sigmoid
        gate_1 = 1.0 - sigmoid

        # Expert inference
        expert_results = []
        for k in range(2):
            eidx = top2[k]
            hidden = np.maximum(params[f'w1_{eidx}'] @ x + params[f'b1_{eidx}'], 0)
            out = params[f'w2_{eidx}'] @ hidden + params[f'b2_{eidx}'][0]
            expert_results.append(out)

        predictions[i] = gate_0 * expert_results[0] + gate_1 * expert_results[1]

    return predictions


# =============================================================================
# Export SystemVerilog header
# =============================================================================
def fmt_sv(val):
    """Format a quantized value as a SystemVerilog signed literal."""
    q = quantize(val)
    if q >= 0:
        return f"16'sd{q}"
    else:
        return f"-16'sd{-q}"


def export_svh(params, filepath):
    """Export weights as SystemVerilog header (moe_weights.svh)."""
    with open(filepath, 'w') as f:
        f.write("// Auto-generated by train_moe.py — DO NOT EDIT\n")
        f.write("// MoE weights in ap_fixed<16,8> format (raw int16)\n")
        f.write(f"// Total parameters: {count_params()}\n\n")

        # Router weights [8][8]
        f.write("localparam logic signed [15:0] ROUTER_W [0:7][0:7] = '{\n")
        for e in range(NUM_EXPERTS):
            vals = ", ".join(fmt_sv(params['router_w'][e, fi]) for fi in range(NUM_FEATURES))
            comma = "," if e < NUM_EXPERTS - 1 else ""
            f.write(f"    '{{{vals}}}{comma}\n")
        f.write("};\n\n")

        # Router biases [8]
        vals = ", ".join(fmt_sv(params['router_b'][e]) for e in range(NUM_EXPERTS))
        f.write(f"localparam logic signed [15:0] ROUTER_B [0:7] = '{{{vals}}};\n\n")

        # Expert weights — separate arrays per expert
        for e in range(NUM_EXPERTS):
            f.write(f"localparam logic signed [15:0] E{e}_W1 [0:15][0:7] = '{{\n")
            for h in range(HIDDEN_DIM):
                vals = ", ".join(fmt_sv(params[f'w1_{e}'][h, fi]) for fi in range(NUM_FEATURES))
                comma = "," if h < HIDDEN_DIM - 1 else ""
                f.write(f"    '{{{vals}}}{comma}\n")
            f.write("};\n")

            vals = ", ".join(fmt_sv(params[f'b1_{e}'][h]) for h in range(HIDDEN_DIM))
            f.write(f"localparam logic signed [15:0] E{e}_B1 [0:15] = '{{{vals}}};\n")

            vals = ", ".join(fmt_sv(params[f'w2_{e}'][h]) for h in range(HIDDEN_DIM))
            f.write(f"localparam logic signed [15:0] E{e}_W2 [0:15] = '{{{vals}}};\n")

            f.write(f"localparam logic signed [15:0] E{e}_B2 = {fmt_sv(params[f'b2_{e}'][0])};\n\n")

    print(f"  Written: {filepath}")


# =============================================================================
# Export C++ header
# =============================================================================
def export_weights_h(params, filepath):
    """Export weights as C++ header (weights.h)."""
    with open(filepath, 'w') as f:
        f.write("// Auto-generated by train_moe.py — DO NOT EDIT\n")
        f.write("// MoE weights in ap_fixed<16,8> format (raw int16)\n")
        f.write(f"// Total parameters: {count_params()}\n\n")
        f.write("#pragma once\n")
        f.write("#include <cstdint>\n\n")

        # Router weights [8][8]
        f.write("constexpr int16_t TRAINED_ROUTER_W[8][8] = {\n")
        for e in range(NUM_EXPERTS):
            vals = ", ".join(str(quantize(params['router_w'][e, fi])) for fi in range(NUM_FEATURES))
            comma = "," if e < NUM_EXPERTS - 1 else ""
            f.write(f"    {{{vals}}}{comma}\n")
        f.write("};\n\n")

        vals = ", ".join(str(quantize(params['router_b'][e])) for e in range(NUM_EXPERTS))
        f.write(f"constexpr int16_t TRAINED_ROUTER_B[8] = {{{vals}}};\n\n")

        f.write("constexpr int16_t TRAINED_EXPERT_W1[8][16][8] = {\n")
        for e in range(NUM_EXPERTS):
            f.write("    {\n")
            for h in range(HIDDEN_DIM):
                vals = ", ".join(str(quantize(params[f'w1_{e}'][h, fi])) for fi in range(NUM_FEATURES))
                comma = "," if h < HIDDEN_DIM - 1 else ""
                f.write(f"        {{{vals}}}{comma}\n")
            comma = "," if e < NUM_EXPERTS - 1 else ""
            f.write(f"    }}{comma}\n")
        f.write("};\n\n")

        f.write("constexpr int16_t TRAINED_EXPERT_B1[8][16] = {\n")
        for e in range(NUM_EXPERTS):
            vals = ", ".join(str(quantize(params[f'b1_{e}'][h])) for h in range(HIDDEN_DIM))
            comma = "," if e < NUM_EXPERTS - 1 else ""
            f.write(f"    {{{vals}}}{comma}\n")
        f.write("};\n\n")

        f.write("constexpr int16_t TRAINED_EXPERT_W2[8][16] = {\n")
        for e in range(NUM_EXPERTS):
            vals = ", ".join(str(quantize(params[f'w2_{e}'][h])) for h in range(HIDDEN_DIM))
            comma = "," if e < NUM_EXPERTS - 1 else ""
            f.write(f"    {{{vals}}}{comma}\n")
        f.write("};\n\n")

        vals = ", ".join(str(quantize(params[f'b2_{e}'][0])) for e in range(NUM_EXPERTS))
        f.write(f"constexpr int16_t TRAINED_EXPERT_B2[8] = {{{vals}}};\n")

    print(f"  Written: {filepath}")


def count_params():
    total = NUM_EXPERTS * NUM_FEATURES  # router_w
    total += NUM_EXPERTS                 # router_b
    total += NUM_EXPERTS * HIDDEN_DIM * NUM_FEATURES  # w1
    total += NUM_EXPERTS * HIDDEN_DIM    # b1
    total += NUM_EXPERTS * HIDDEN_DIM    # w2
    total += NUM_EXPERTS                 # b2
    return total


# =============================================================================
# Main
# =============================================================================
def main():
    print("=== MoE Weight Training ===")
    print(f"  Features:  {NUM_FEATURES}")
    print(f"  Experts:   {NUM_EXPERTS}")
    print(f"  Hidden:    {HIDDEN_DIM}")
    print(f"  Top-K:     {TOP_K}")
    print(f"  Params:    {count_params()}")
    print()

    # Generate data
    print("Generating synthetic training data...")
    X, y = generate_synthetic_data(NUM_SAMPLES, seed=SEED)
    buy_pct = (y > 0).mean() * 100
    sell_pct = (y < 0).mean() * 100
    hold_pct = (y == 0).mean() * 100
    print(f"  Samples: {NUM_SAMPLES}")
    print(f"  Labels:  Buy={buy_pct:.1f}%  Sell={sell_pct:.1f}%  Hold={hold_pct:.1f}%")
    print()

    # Train
    print("Training MoE model (soft gating)...")
    params = train_moe(X, y, num_epochs=NUM_EPOCHS, batch_size=BATCH_SIZE,
                       lr=LEARNING_RATE, seed=SEED)
    print()

    # Evaluate with top-2 gating (matching hardware)
    preds = evaluate_top2(X, params)
    pred_buy = (preds > 0.1).sum()
    pred_sell = (preds < -0.1).sum()
    pred_hold = X.shape[0] - pred_buy - pred_sell
    print(f"Top-2 predictions: Buy={pred_buy}  Sell={pred_sell}  Hold={pred_hold}")

    all_weights = np.concatenate([params[k].flatten() for k in params])
    print(f"Weight range: [{all_weights.min():.4f}, {all_weights.max():.4f}]")
    print()

    # Export
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)

    svh_path = os.path.join(project_dir, "src", "rtl", "moe_weights.svh")
    h_path = os.path.join(project_dir, "src", "golden_model", "weights.h")

    print("Exporting weights...")
    export_svh(params, svh_path)
    export_weights_h(params, h_path)
    print()
    print("Done! Weight files generated successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
