// =============================================================================
// Matching Engine — Implementation
// =============================================================================

#include "matching_engine.h"

void matching_engine(
    hls::stream<OrderInput>&  orders_in,
    hls::stream<MatchResult>& matches_out,
    LOBState&                 lob
) {
    #pragma HLS INTERFACE axis port=orders_in
    #pragma HLS INTERFACE axis port=matches_out
    #pragma HLS INTERFACE bram port=lob.bid_levels
    #pragma HLS INTERFACE bram port=lob.ask_levels
    #pragma HLS PIPELINE II=1

    // Partition BRAM for parallel access to different price levels
    #pragma HLS ARRAY_PARTITION variable=lob.bid_levels cyclic factor=4
    #pragma HLS ARRAY_PARTITION variable=lob.ask_levels cyclic factor=4

    OrderInput order;
    if (!orders_in.read_nb(order)) return;
    if (!order.valid) return;

    MatchResult result;
    result.matched = false;
    result.taker_ref = order.order_ref;

    ap_uint<32> price_idx = order.price;

    // Bounds check
    if (price_idx >= MAX_PRICE_LEVELS) {
        matches_out.write(result);
        return;
    }

    if (order.side == 0) {
        // =====================================================================
        // BUY ORDER
        // =====================================================================
        // Check if it crosses the best ask
        if (lob.best_ask != 0 && order.price >= lob.best_ask) {
            // Match against the best ask
            ap_uint<32> ask_qty = lob.ask_levels[lob.best_ask];
            ap_uint<32> match_qty = (order.quantity < ask_qty)
                                    ? order.quantity : ask_qty;

            result.matched       = true;
            result.match_price   = lob.best_ask;
            result.match_quantity = match_qty;

            // Update ask level
            lob.ask_levels[lob.best_ask] -= match_qty;

            // If ask level is now empty, find next best ask
            if (lob.ask_levels[lob.best_ask] == 0) {
                FIND_NEXT_ASK:
                for (int i = lob.best_ask + 1; i < MAX_PRICE_LEVELS; i++) {
                    #pragma HLS PIPELINE II=1
                    if (lob.ask_levels[i] > 0) {
                        lob.best_ask = i;
                        break;
                    }
                    if (i == MAX_PRICE_LEVELS - 1) {
                        lob.best_ask = 0;  // No more asks
                    }
                }
            }

            // Remaining quantity goes to bid book
            ap_uint<32> remaining = order.quantity - match_qty;
            if (remaining > 0) {
                lob.bid_levels[price_idx] += remaining;
                if (price_idx > lob.best_bid || lob.best_bid == 0) {
                    lob.best_bid = price_idx;
                }
            }
        } else {
            // No cross — add to bid book
            lob.bid_levels[price_idx] += order.quantity;
            if (price_idx > lob.best_bid || lob.best_bid == 0) {
                lob.best_bid = price_idx;
            }
        }
    } else {
        // =====================================================================
        // SELL ORDER
        // =====================================================================
        if (lob.best_bid != 0 && order.price <= lob.best_bid) {
            // Match against the best bid
            ap_uint<32> bid_qty = lob.bid_levels[lob.best_bid];
            ap_uint<32> match_qty = (order.quantity < bid_qty)
                                    ? order.quantity : bid_qty;

            result.matched       = true;
            result.match_price   = lob.best_bid;
            result.match_quantity = match_qty;

            // Update bid level
            lob.bid_levels[lob.best_bid] -= match_qty;

            // If bid level is now empty, find next best bid
            if (lob.bid_levels[lob.best_bid] == 0) {
                FIND_NEXT_BID:
                for (int i = lob.best_bid - 1; i >= 0; i--) {
                    #pragma HLS PIPELINE II=1
                    if (lob.bid_levels[i] > 0) {
                        lob.best_bid = i;
                        break;
                    }
                    if (i == 0) {
                        lob.best_bid = 0;  // No more bids
                    }
                }
            }

            // Remaining quantity goes to ask book
            ap_uint<32> remaining = order.quantity - match_qty;
            if (remaining > 0) {
                lob.ask_levels[price_idx] += remaining;
                if (price_idx < lob.best_ask || lob.best_ask == 0) {
                    lob.best_ask = price_idx;
                }
            }
        } else {
            // No cross — add to ask book
            lob.ask_levels[price_idx] += order.quantity;
            if (price_idx < lob.best_ask || lob.best_ask == 0) {
                lob.best_ask = price_idx;
            }
        }
    }

    matches_out.write(result);
}
