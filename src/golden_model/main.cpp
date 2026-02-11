// =============================================================================
// Golden Model — C++ Reference Implementation
// =============================================================================
// Provides a bit-accurate software implementation of the full pipeline:
//   1. ITCH 5.0 parser
//   2. Feature extraction
//   3. MoE routing + expert inference
//   4. Limit Order Book matching
//
// This serves as the ground truth for Verilator verification.

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <queue>
#include <map>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cassert>

// =============================================================================
// Fixed-Point Emulation
// =============================================================================
// Matches ap_fixed<16,8>: 16 bits, 8 integer, 8 fractional
class FixedPoint {
    int16_t raw;
    static constexpr int FRAC_BITS = 8;
    static constexpr int SCALE = 1 << FRAC_BITS; // 256

public:
    FixedPoint() : raw(0) {}
    explicit FixedPoint(double val) : raw(static_cast<int16_t>(val * SCALE)) {}
    explicit FixedPoint(int16_t raw_val, bool) : raw(raw_val) {} // raw constructor

    double to_double() const { return static_cast<double>(raw) / SCALE; }
    int16_t get_raw() const { return raw; }

    FixedPoint operator+(const FixedPoint& o) const {
        return FixedPoint(static_cast<int16_t>(raw + o.raw), true);
    }
    FixedPoint operator-(const FixedPoint& o) const {
        return FixedPoint(static_cast<int16_t>(raw - o.raw), true);
    }
    FixedPoint operator*(const FixedPoint& o) const {
        int32_t result = (static_cast<int32_t>(raw) * o.raw) >> FRAC_BITS;
        return FixedPoint(static_cast<int16_t>(result), true);
    }
    bool operator>(const FixedPoint& o) const { return raw > o.raw; }
    bool operator<(const FixedPoint& o) const { return raw < o.raw; }

    static FixedPoint relu(const FixedPoint& x) {
        return x.raw > 0 ? x : FixedPoint();
    }
};

// =============================================================================
// ITCH 5.0 Structures
// =============================================================================
constexpr uint8_t ITCH_ADD_ORDER = 0x41; // 'A'

struct ITCHAddOrder {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;       // nanoseconds since midnight
    uint64_t order_ref;
    char     side;            // 'B' or 'S'
    uint32_t shares;
    char     stock[9];        // null-terminated
    uint32_t price;           // 4 implied decimal places
};

// =============================================================================
// ITCH Parser
// =============================================================================
class ITCHParser {
public:
    struct Stats {
        uint64_t total_messages = 0;
        uint64_t add_orders = 0;
    };

    // Parse a raw ITCH message buffer. Returns true if it's an Add Order.
    bool parse(const uint8_t* data, size_t len, ITCHAddOrder& order) {
        stats.total_messages++;

        if (len < 1) return false;
        if (data[0] != ITCH_ADD_ORDER) return false;
        if (len < 36) return false;

        stats.add_orders++;

        // Big-endian extraction (ITCH is network byte order)
        order.stock_locate    = (data[1] << 8) | data[2];
        order.tracking_number = (data[3] << 8) | data[4];

        order.timestamp = 0;
        for (int i = 0; i < 6; i++) {
            order.timestamp = (order.timestamp << 8) | data[5 + i];
        }

        order.order_ref = 0;
        for (int i = 0; i < 8; i++) {
            order.order_ref = (order.order_ref << 8) | data[11 + i];
        }

        order.side = static_cast<char>(data[19]);

        order.shares = 0;
        for (int i = 0; i < 4; i++) {
            order.shares = (order.shares << 8) | data[20 + i];
        }

        std::memcpy(order.stock, &data[24], 8);
        order.stock[8] = '\0';

        order.price = 0;
        for (int i = 0; i < 4; i++) {
            order.price = (order.price << 8) | data[32 + i];
        }

        return true;
    }

    const Stats& get_stats() const { return stats; }

private:
    Stats stats;
};

// =============================================================================
// MoE Configuration
// =============================================================================
constexpr int NUM_FEATURES = 8;
constexpr int NUM_EXPERTS  = 8;
constexpr int TOP_K        = 2;
constexpr int HIDDEN_DIM   = 16;

// =============================================================================
// Feature Extractor
// =============================================================================
// Converts raw order book state into a fixed-point feature vector
struct FeatureVector {
    FixedPoint features[NUM_FEATURES];
};

class FeatureExtractor {
public:
    FeatureVector extract(const ITCHAddOrder& order, uint32_t best_bid, uint32_t best_ask) {
        FeatureVector fv;

        // Feature 0: Normalized price (relative to midpoint)
        double mid = (best_bid + best_ask) / 2.0;
        fv.features[0] = FixedPoint(mid > 0 ? (order.price - mid) / mid : 0.0);

        // Feature 1: Side indicator (-1 for sell, +1 for buy)
        fv.features[1] = FixedPoint(order.side == 'B' ? 1.0 : -1.0);

        // Feature 2: Log quantity (normalized)
        fv.features[2] = FixedPoint(std::log2(std::max(1u, order.shares)) / 16.0);

        // Feature 3: Spread (normalized)
        double spread = (best_ask > best_bid) ? (best_ask - best_bid) : 0;
        fv.features[3] = FixedPoint(spread / 10000.0);

        // Feature 4: Price level distance from best
        double dist = order.side == 'B'
                      ? (best_bid > 0 ? (double)(best_bid - order.price) / best_bid : 0)
                      : (best_ask > 0 ? (double)(order.price - best_ask) / best_ask : 0);
        fv.features[4] = FixedPoint(dist);

        // Features 5-7: Reserved / rolling statistics (zero for now)
        fv.features[5] = FixedPoint(0.0);
        fv.features[6] = FixedPoint(0.0);
        fv.features[7] = FixedPoint(0.0);

        return fv;
    }
};

// =============================================================================
// MoE Model
// =============================================================================
struct ExpertWeights {
    FixedPoint w1[HIDDEN_DIM][NUM_FEATURES];
    FixedPoint b1[HIDDEN_DIM];
    FixedPoint w2[HIDDEN_DIM];
    FixedPoint b2;
};

class MoEModel {
public:
    FixedPoint router_weights[NUM_EXPERTS][NUM_FEATURES];
    FixedPoint router_biases[NUM_EXPERTS];
    ExpertWeights experts[NUM_EXPERTS];

    MoEModel() {
        // Initialize with small random-ish weights for demonstration
        // In production, these would be loaded from a trained model
        for (int e = 0; e < NUM_EXPERTS; e++) {
            for (int f = 0; f < NUM_FEATURES; f++) {
                double w = 0.1 * ((e * NUM_FEATURES + f) % 7 - 3) / 3.0;
                router_weights[e][f] = FixedPoint(w);
            }
            router_biases[e] = FixedPoint(0.01 * e);

            for (int h = 0; h < HIDDEN_DIM; h++) {
                for (int f = 0; f < NUM_FEATURES; f++) {
                    double w = 0.1 * ((e * h + f) % 11 - 5) / 5.0;
                    experts[e].w1[h][f] = FixedPoint(w);
                }
                experts[e].b1[h] = FixedPoint(0.0);
                experts[e].w2[h] = FixedPoint(0.05 * ((e + h) % 5 - 2));
            }
            experts[e].b2 = FixedPoint(0.0);
        }
    }

    struct TradeSignal {
        uint8_t  action;     // 0=Hold, 1=Buy, 2=Sell
        double   confidence;
        uint32_t price;
        uint32_t quantity;
    };

    TradeSignal infer(const FeatureVector& fv) {
        // Step 1: Router scores
        double scores[NUM_EXPERTS];
        for (int e = 0; e < NUM_EXPERTS; e++) {
            FixedPoint sum = router_biases[e];
            for (int f = 0; f < NUM_FEATURES; f++) {
                sum = sum + router_weights[e][f] * fv.features[f];
            }
            scores[e] = sum.to_double();
        }

        // Step 2: Top-K selection
        int top_idx[TOP_K] = {0, 1};
        double top_scores[TOP_K] = {scores[0], scores[1]};
        for (int e = 0; e < NUM_EXPERTS; e++) {
            if (scores[e] > top_scores[0]) {
                top_scores[1] = top_scores[0];
                top_idx[1] = top_idx[0];
                top_scores[0] = scores[e];
                top_idx[0] = e;
            } else if (scores[e] > top_scores[1]) {
                top_scores[1] = scores[e];
                top_idx[1] = e;
            }
        }

        // Step 3: Softmax (piecewise linear, matching HLS)
        double diff = top_scores[0] - top_scores[1];
        double sigmoid;
        if (diff > 2.0)       sigmoid = 1.0;
        else if (diff < -2.0) sigmoid = 0.0;
        else                   sigmoid = 0.5 + 0.25 * diff;

        double gate_0 = sigmoid;
        double gate_1 = 1.0 - sigmoid;

        // Step 4: Expert inference
        double expert_results[TOP_K];
        for (int k = 0; k < TOP_K; k++) {
            int eidx = top_idx[k];

            // Layer 1
            FixedPoint hidden[HIDDEN_DIM];
            for (int h = 0; h < HIDDEN_DIM; h++) {
                FixedPoint sum = experts[eidx].b1[h];
                for (int f = 0; f < NUM_FEATURES; f++) {
                    sum = sum + experts[eidx].w1[h][f] * fv.features[f];
                }
                hidden[h] = FixedPoint::relu(sum);
            }

            // Layer 2
            FixedPoint out = experts[eidx].b2;
            for (int h = 0; h < HIDDEN_DIM; h++) {
                out = out + experts[eidx].w2[h] * hidden[h];
            }
            expert_results[k] = out.to_double();
        }

        // Step 5: Weighted combination
        double combined = gate_0 * expert_results[0] + gate_1 * expert_results[1];

        // Step 6: Decision
        TradeSignal signal;
        signal.confidence = std::abs(combined);
        if (combined > 0.1) {
            signal.action = 1; // Buy
        } else if (combined < -0.1) {
            signal.action = 2; // Sell
        } else {
            signal.action = 0; // Hold
        }
        signal.price = 0;
        signal.quantity = 0;

        return signal;
    }
};

// =============================================================================
// Limit Order Book (Software Reference)
// =============================================================================
class OrderBook {
    std::map<uint32_t, uint64_t, std::greater<uint32_t>> bids; // price → qty (descending)
    std::map<uint32_t, uint64_t>                          asks; // price → qty (ascending)

public:
    uint32_t get_best_bid() const {
        return bids.empty() ? 0 : bids.begin()->first;
    }
    uint32_t get_best_ask() const {
        return asks.empty() ? 0 : asks.begin()->first;
    }

    struct Match {
        bool     matched;
        uint32_t price;
        uint32_t quantity;
    };

    Match add_order(char side, uint32_t price, uint32_t quantity) {
        Match m = {false, 0, 0};

        if (side == 'B') {
            if (!asks.empty() && price >= asks.begin()->first) {
                uint32_t ask_price = asks.begin()->first;
                uint64_t& ask_qty = asks.begin()->second;
                uint32_t match_qty = std::min((uint64_t)quantity, ask_qty);

                m.matched = true;
                m.price = ask_price;
                m.quantity = match_qty;

                ask_qty -= match_qty;
                if (ask_qty == 0) asks.erase(asks.begin());

                uint32_t remaining = quantity - match_qty;
                if (remaining > 0) bids[price] += remaining;
            } else {
                bids[price] += quantity;
            }
        } else {
            if (!bids.empty() && price <= bids.begin()->first) {
                uint32_t bid_price = bids.begin()->first;
                uint64_t& bid_qty = bids.begin()->second;
                uint32_t match_qty = std::min((uint64_t)quantity, bid_qty);

                m.matched = true;
                m.price = bid_price;
                m.quantity = match_qty;

                bid_qty -= match_qty;
                if (bid_qty == 0) bids.erase(bids.begin());

                uint32_t remaining = quantity - match_qty;
                if (remaining > 0) asks[price] += remaining;
            } else {
                asks[price] += quantity;
            }
        }

        return m;
    }
};

// =============================================================================
// Main — Golden Model Driver
// =============================================================================
int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "=== FPGA MoE Trading Engine — Golden Model ===" << std::endl;
    std::cout << "Building reference outputs for Verilator verification" << std::endl;
    std::cout << std::endl;

    ITCHParser parser;
    FeatureExtractor fe;
    MoEModel model;
    OrderBook ob;

    // -------------------------------------------------------------------------
    // Generate synthetic ITCH Add Order messages for testing
    // -------------------------------------------------------------------------
    struct TestOrder {
        char     side;
        uint32_t price;
        uint32_t shares;
        const char* stock;
    };

    std::vector<TestOrder> test_orders = {
        {'B', 100000, 100, "AAPL    "},  // Buy  AAPL @ $10.0000
        {'S', 100100, 200, "AAPL    "},  // Sell AAPL @ $10.0100
        {'B', 100050, 150, "AAPL    "},  // Buy  AAPL @ $10.0050
        {'S', 100050, 50,  "AAPL    "},  // Sell AAPL @ $10.0050 (should match!)
        {'B', 100200, 300, "AAPL    "},  // Buy  AAPL @ $10.0200 (crosses ask!)
        {'S', 99900,  100, "AAPL    "},  // Sell AAPL @ $9.9900  (crosses bid!)
        {'B', 100000, 500, "GOOG    "},  // Buy  GOOG @ $10.0000
        {'S', 100500, 250, "GOOG    "},  // Sell GOOG @ $10.0500
    };

    std::cout << "Processing " << test_orders.size() << " synthetic orders:" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    // Output file for Verilator comparison
    std::ofstream trace("golden_trace.csv");
    trace << "order_idx,side,price,shares,stock,best_bid,best_ask,moe_action,moe_confidence,matched,match_price,match_qty" << std::endl;

    for (size_t i = 0; i < test_orders.size(); i++) {
        auto& t = test_orders[i];

        // Build raw ITCH message
        uint8_t msg[36] = {};
        msg[0] = ITCH_ADD_ORDER;
        // stock_locate, tracking_number = 0
        // timestamp = i * 1000ns
        uint64_t ts = i * 1000;
        for (int b = 0; b < 6; b++) msg[5 + b] = (ts >> (40 - 8*b)) & 0xFF;
        // order_ref = i
        for (int b = 0; b < 8; b++) msg[11 + b] = (i >> (56 - 8*b)) & 0xFF;
        msg[19] = t.side;
        for (int b = 0; b < 4; b++) msg[20 + b] = (t.shares >> (24 - 8*b)) & 0xFF;
        std::memcpy(&msg[24], t.stock, 8);
        for (int b = 0; b < 4; b++) msg[32 + b] = (t.price >> (24 - 8*b)) & 0xFF;

        // Parse
        ITCHAddOrder order;
        bool is_add = parser.parse(msg, 36, order);
        assert(is_add);

        // Get current book state
        uint32_t bb = ob.get_best_bid();
        uint32_t ba = ob.get_best_ask();

        // Feature extraction + MoE inference
        FeatureVector fv = fe.extract(order, bb, ba);
        auto signal = model.infer(fv);

        // Order book update
        auto match = ob.add_order(t.side, t.price, t.shares);

        // Log
        std::cout << "Order " << i << ": " << t.side << " "
                  << t.shares << " shares @ $" << std::fixed << std::setprecision(4)
                  << (t.price / 10000.0)
                  << " | MoE: " << (signal.action == 0 ? "HOLD" : signal.action == 1 ? "BUY " : "SELL")
                  << " (conf=" << std::setprecision(4) << signal.confidence << ")"
                  << " | Match: " << (match.matched ? "YES" : "NO ");
        if (match.matched) {
            std::cout << " @ $" << std::setprecision(4) << (match.price / 10000.0)
                      << " x" << match.quantity;
        }
        std::cout << std::endl;

        // Write trace
        trace << i << "," << t.side << "," << t.price << "," << t.shares << ","
              << t.stock << "," << bb << "," << ba << ","
              << (int)signal.action << "," << signal.confidence << ","
              << match.matched << "," << match.price << "," << match.quantity
              << std::endl;
    }

    std::cout << std::string(70, '-') << std::endl;
    std::cout << "Total messages parsed: " << parser.get_stats().total_messages << std::endl;
    std::cout << "Add orders parsed:     " << parser.get_stats().add_orders << std::endl;
    std::cout << "Final best bid: $" << std::fixed << std::setprecision(4)
              << (ob.get_best_bid() / 10000.0) << std::endl;
    std::cout << "Final best ask: $" << std::setprecision(4)
              << (ob.get_best_ask() / 10000.0) << std::endl;
    std::cout << "\nGolden trace written to: golden_trace.csv" << std::endl;

    return 0;
}
