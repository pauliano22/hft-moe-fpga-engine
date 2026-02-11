// =============================================================================
// Matching Engine — Hardware Limit Order Book (Header)
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Defines the data structures and interface for a hardware-accelerated
//   Limit Order Book (LOB). The LOB is the core data structure of any
//   exchange — it tracks all outstanding buy and sell orders.
//
// HOW A LIMIT ORDER BOOK WORKS:
//   Imagine two sides of a marketplace:
//
//     BUYERS (Bids)              SELLERS (Asks)
//     Want to buy at ≤ price     Want to sell at ≥ price
//     ─────────────────          ─────────────────
//     $150.00 × 100 shares      $150.05 × 200 shares  ← Best Ask
//     $149.95 × 300 shares      $150.10 × 500 shares
//     $149.90 × 200 shares      $150.15 × 150 shares
//         ↑
//     Best Bid (highest)
//
//   The "spread" = Best Ask - Best Bid = $150.05 - $150.00 = $0.05
//   A "match" happens when a new order's price crosses the spread.
//
// HARDWARE DESIGN:
//   Instead of sorted lists (like the software model uses), we use the price
//   as a direct BRAM address. BRAM[1500] holds the quantity at price $15.00.
//   This gives O(1) lookup — no searching through sorted data.
//
//   Trade-off: We can only cover MAX_PRICE_LEVELS distinct prices.
//   4096 levels at $0.01 tick size = ~$40 range, which is enough for
//   most liquid securities during a trading session.

#ifndef MATCHING_ENGINE_H
#define MATCHING_ENGINE_H

#include "../moe_router/moe_router.h"  // For ap_uint, fixed_t, hls::stream

// =============================================================================
// LOB Configuration
// =============================================================================
constexpr int MAX_PRICE_LEVELS = 4096;  // 4K price levels in BRAM
constexpr int PRICE_OFFSET     = 2048;  // Center price (allows ±$20.48 range)

// =============================================================================
// Order Input — What enters the matching engine
// =============================================================================
struct OrderInput {
    ap_uint<1>  side;       // 0 = Buy (bid), 1 = Sell (ask)
    ap_uint<32> price;      // Price in tick units (e.g., 15000 = $150.00)
    ap_uint<32> quantity;   // Number of shares
    ap_uint<64> order_ref;  // Unique order identifier from ITCH
    bool        valid;      // True when this order should be processed
};

// =============================================================================
// Match Output — What the matching engine reports
// =============================================================================
struct MatchResult {
    bool        matched;         // True if a trade occurred
    ap_uint<32> match_price;     // Price at which the trade happened
    ap_uint<32> match_quantity;  // How many shares were traded
    ap_uint<64> maker_ref;       // Order ref of the resting order (maker)
    ap_uint<64> taker_ref;       // Order ref of the incoming order (taker)
};

// =============================================================================
// LOB State — Stored in BRAM
// =============================================================================
// This is the actual order book data structure. In hardware, each array
// maps to a separate BRAM block, giving parallel read/write access.
//
// bid_levels[i] = total shares at bid price level i
// ask_levels[i] = total shares at ask price level i
// best_bid = highest price with bid quantity > 0
// best_ask = lowest price with ask quantity > 0
struct LOBState {
    ap_uint<32> bid_levels[MAX_PRICE_LEVELS];  // Quantity at each bid price
    ap_uint<32> ask_levels[MAX_PRICE_LEVELS];  // Quantity at each ask price
    ap_uint<32> best_bid;                       // Current highest bid price
    ap_uint<32> best_ask;                       // Current lowest ask price
};

// =============================================================================
// Top-Level Function
// =============================================================================
void matching_engine(
    hls::stream<OrderInput>&  orders_in,     // Incoming orders (AXI-Stream)
    hls::stream<MatchResult>& matches_out,   // Match results (AXI-Stream)
    LOBState&                 lob            // Order book state (BRAM)
);

#endif // MATCHING_ENGINE_H
