// =============================================================================
// Matching Engine — Implementation
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Implements the order matching logic for the hardware Limit Order Book.
//   When a new order arrives, this module checks if it can trade against
//   existing orders on the opposite side of the book.
//
// MATCHING ALGORITHM:
//   For a BUY order at price P:
//     1. If P >= best_ask → MATCH: trade at best_ask price
//        - Reduce ask quantity, find next best ask if exhausted
//        - Remaining buy quantity goes to bid book
//     2. If P < best_ask → NO MATCH: add to bid book
//
//   For a SELL order at price P:
//     1. If P <= best_bid → MATCH: trade at best_bid price
//        - Reduce bid quantity, find next best bid if exhausted
//        - Remaining sell quantity goes to ask book
//     2. If P > best_bid → NO MATCH: add to ask book
//
// SIMPLIFICATIONS FOR DEMONSTRATION:
//   - Only matches against the single best price level (real LOBs match
//     through multiple levels when a large order sweeps the book)
//   - No order queues per price level (just aggregate quantity)
//   - No order cancellation or modification support

#include "matching_engine.h"

void matching_engine(
    hls::stream<OrderInput>&  orders_in,
    hls::stream<MatchResult>& matches_out,
    LOBState&                 lob
) {
    // =========================================================================
    // HLS Interface Pragmas
    // =========================================================================
    #pragma HLS INTERFACE axis port=orders_in    // Orders arrive as AXI-Stream
    #pragma HLS INTERFACE axis port=matches_out  // Match results as AXI-Stream
    #pragma HLS INTERFACE bram port=lob.bid_levels  // Bid book in BRAM
    #pragma HLS INTERFACE bram port=lob.ask_levels  // Ask book in BRAM

    // Pipeline the main function for throughput
    #pragma HLS PIPELINE II=1

    // Partition BRAM arrays so multiple price levels can be accessed per cycle.
    // "cyclic factor=4" means: distribute elements across 4 BRAM banks in
    // round-robin fashion. This allows 4 simultaneous reads/writes.
    #pragma HLS ARRAY_PARTITION variable=lob.bid_levels cyclic factor=4
    #pragma HLS ARRAY_PARTITION variable=lob.ask_levels cyclic factor=4

    // -------------------------------------------------------------------------
    // Read incoming order
    // -------------------------------------------------------------------------
    OrderInput order;
    if (!orders_in.read_nb(order)) return;  // Non-blocking: skip if no data
    if (!order.valid) return;                // Skip invalid orders

    // Initialize match result (default: no match)
    MatchResult result;
    result.matched = false;
    result.taker_ref = order.order_ref;

    ap_uint<32> price_idx = order.price;

    // Bounds check: reject orders outside our price range
    if (price_idx >= MAX_PRICE_LEVELS) {
        matches_out.write(result);
        return;
    }

    if (order.side == 0) {
        // =================================================================
        // BUY ORDER
        // =================================================================
        // A buy order "crosses" when its price >= the best ask price.
        // This means the buyer is willing to pay at least what the cheapest
        // seller is asking — a trade can happen.
        if (lob.best_ask != 0 && order.price >= lob.best_ask) {
            // --- MATCH: Trade at the best ask price ---
            ap_uint<32> ask_qty = lob.ask_levels[lob.best_ask];
            ap_uint<32> match_qty = (order.quantity < ask_qty)
                                    ? order.quantity : ask_qty;

            result.matched       = true;
            result.match_price   = lob.best_ask;
            result.match_quantity = match_qty;

            // Reduce the ask level by the matched quantity
            lob.ask_levels[lob.best_ask] -= match_qty;

            // If this ask price level is now empty, find the next best ask
            // (the next lowest price with quantity > 0)
            if (lob.ask_levels[lob.best_ask] == 0) {
                FIND_NEXT_ASK:
                for (int i = lob.best_ask + 1; i < MAX_PRICE_LEVELS; i++) {
                    #pragma HLS PIPELINE II=1
                    if (lob.ask_levels[i] > 0) {
                        lob.best_ask = i;
                        break;
                    }
                    if (i == MAX_PRICE_LEVELS - 1) {
                        lob.best_ask = 0;  // Book is empty on the ask side
                    }
                }
            }

            // If the buy order wasn't fully filled, add remainder to bid book
            ap_uint<32> remaining = order.quantity - match_qty;
            if (remaining > 0) {
                lob.bid_levels[price_idx] += remaining;
                if (price_idx > lob.best_bid || lob.best_bid == 0) {
                    lob.best_bid = price_idx;  // Update best bid if higher
                }
            }
        } else {
            // --- NO MATCH: Just add to the bid book ---
            lob.bid_levels[price_idx] += order.quantity;
            if (price_idx > lob.best_bid || lob.best_bid == 0) {
                lob.best_bid = price_idx;
            }
        }
    } else {
        // =================================================================
        // SELL ORDER
        // =================================================================
        // A sell order "crosses" when its price <= the best bid price.
        if (lob.best_bid != 0 && order.price <= lob.best_bid) {
            // --- MATCH: Trade at the best bid price ---
            ap_uint<32> bid_qty = lob.bid_levels[lob.best_bid];
            ap_uint<32> match_qty = (order.quantity < bid_qty)
                                    ? order.quantity : bid_qty;

            result.matched       = true;
            result.match_price   = lob.best_bid;
            result.match_quantity = match_qty;

            // Reduce the bid level by the matched quantity
            lob.bid_levels[lob.best_bid] -= match_qty;

            // If this bid price level is now empty, find the next best bid
            // (the next highest price with quantity > 0)
            if (lob.bid_levels[lob.best_bid] == 0) {
                FIND_NEXT_BID:
                for (int i = lob.best_bid - 1; i >= 0; i--) {
                    #pragma HLS PIPELINE II=1
                    if (lob.bid_levels[i] > 0) {
                        lob.best_bid = i;
                        break;
                    }
                    if (i == 0) {
                        lob.best_bid = 0;  // Book is empty on the bid side
                    }
                }
            }

            // If the sell order wasn't fully filled, add remainder to ask book
            ap_uint<32> remaining = order.quantity - match_qty;
            if (remaining > 0) {
                lob.ask_levels[price_idx] += remaining;
                if (price_idx < lob.best_ask || lob.best_ask == 0) {
                    lob.best_ask = price_idx;  // Update best ask if lower
                }
            }
        } else {
            // --- NO MATCH: Just add to the ask book ---
            lob.ask_levels[price_idx] += order.quantity;
            if (price_idx < lob.best_ask || lob.best_ask == 0) {
                lob.best_ask = price_idx;
            }
        }
    }

    // Write the match result to the output stream
    matches_out.write(result);
}
