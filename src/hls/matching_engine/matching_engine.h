// =============================================================================
// Matching Engine — Hardware Limit Order Book
// =============================================================================
// BRAM-partitioned LOB with O(1) price-level access.
//
// Design:
//   - Price levels are used as direct BRAM addresses
//   - Prices are discretized to tick size (e.g., $0.01 → 1 tick)
//   - Each price level stores aggregate quantity
//   - Best bid/ask tracked with simple registers
//
// This is a simplified model suitable for simulation and demonstration.
// A production LOB would need FIFO queues per price level.

#ifndef MATCHING_ENGINE_H
#define MATCHING_ENGINE_H

#include "../moe_router/moe_router.h"

// =============================================================================
// LOB Configuration
// =============================================================================
constexpr int MAX_PRICE_LEVELS = 4096;  // Covers ~$40 range at $0.01 ticks
constexpr int PRICE_OFFSET     = 2048;  // Center price (allows +/- $20.48)

// =============================================================================
// Order Input
// =============================================================================
struct OrderInput {
    ap_uint<1>  side;       // 0 = Buy, 1 = Sell
    ap_uint<32> price;      // Price in ticks
    ap_uint<32> quantity;   // Number of shares
    ap_uint<64> order_ref;  // Order reference
    bool        valid;
};

// =============================================================================
// Match Output
// =============================================================================
struct MatchResult {
    bool        matched;
    ap_uint<32> match_price;
    ap_uint<32> match_quantity;
    ap_uint<64> maker_ref;
    ap_uint<64> taker_ref;
};

// =============================================================================
// LOB State (stored in BRAM)
// =============================================================================
struct LOBState {
    ap_uint<32> bid_levels[MAX_PRICE_LEVELS];  // Quantity at each bid price
    ap_uint<32> ask_levels[MAX_PRICE_LEVELS];  // Quantity at each ask price
    ap_uint<32> best_bid;                       // Highest bid price
    ap_uint<32> best_ask;                       // Lowest ask price
};

// =============================================================================
// Top-Level Function
// =============================================================================
void matching_engine(
    hls::stream<OrderInput>&  orders_in,
    hls::stream<MatchResult>& matches_out,
    LOBState&                 lob
);

#endif // MATCHING_ENGINE_H
