// =========================================================================
// order_book.cpp — Price-level Limit Order Book implementation
// See order_book.hpp for the design overview.
// =========================================================================

#include "order_book.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace book {

// -------------------------------------------------------------------------
// remove_from_level — find an order in a price level's FIFO queue and remove
// it. Also decrements total_shares for the level. If the level becomes empty
// after removal, erases it from the side map.
//
// Template parameter Map is either:
//   std::map<uint32_t, PriceLevel, std::greater<uint32_t>>  (bids)
//   std::map<uint32_t, PriceLevel>                           (asks)
// -------------------------------------------------------------------------
template<typename Map>
void OrderBook::remove_from_level(Map& side_map, uint32_t price,
                                   uint64_t order_ref_num, uint32_t shares) {
    auto level_it = side_map.find(price);
    if (level_it == side_map.end()) return;

    PriceLevel& level = level_it->second;

    // Linear scan through the order queue at this price level.
    // In practice, price levels in real ITCH data have 1–10 orders on average,
    // so this is effectively O(1) for real workloads.
    auto& q = level.order_refs;
    auto it = std::find(q.begin(), q.end(), order_ref_num);
    if (it != q.end()) {
        q.erase(it);
        // Protect against underflow if our state got out of sync
        level.total_shares -= std::min(shares, level.total_shares);
    }

    // Prune the price level from the map if no orders remain.
    // This keeps best_bid() and best_ask() O(1) — the front of the map is
    // always the best price with at least one live order.
    if (level.order_refs.empty()) {
        side_map.erase(level_it);
    }
}

// -------------------------------------------------------------------------
// add — insert a new resting order
// -------------------------------------------------------------------------
void OrderBook::add(const itch::ParsedAddOrder& msg) {
    // Build the internal Order record
    Order order;
    order.order_ref_num = msg.order_ref_num;
    order.side          = msg.side;
    order.shares        = msg.shares;
    order.price         = msg.price;
    memcpy(order.stock, msg.stock, 9); // includes null terminator

    // Store in primary lookup map
    orders_[msg.order_ref_num] = order;

    // Insert into the appropriate side's price-level map
    if (msg.side == 'B') {
        auto& level = bids_[msg.price];
        level.total_shares += msg.shares;
        level.order_refs.push_back(msg.order_ref_num); // new order goes to back (queue)
    } else {
        auto& level = asks_[msg.price];
        level.total_shares += msg.shares;
        level.order_refs.push_back(msg.order_ref_num);
    }
}

// -------------------------------------------------------------------------
// execute — an order was matched in full or in part
// -------------------------------------------------------------------------
void OrderBook::execute(const itch::ParsedOrderExecuted& msg) {
    auto it = orders_.find(msg.order_ref_num);
    if (it == orders_.end()) return; // unknown order — stale or pre-session

    Order& order = it->second;

    if (order.side == 'B') {
        auto level_it = bids_.find(order.price);
        if (level_it != bids_.end()) {
            level_it->second.total_shares -=
                std::min(msg.executed_shares, level_it->second.total_shares);
        }
    } else {
        auto level_it = asks_.find(order.price);
        if (level_it != asks_.end()) {
            level_it->second.total_shares -=
                std::min(msg.executed_shares, level_it->second.total_shares);
        }
    }

    // Reduce remaining shares on the order
    uint32_t executed = std::min(msg.executed_shares, order.shares);
    order.shares -= executed;

    // If fully executed, remove from the book entirely
    if (order.shares == 0) {
        if (order.side == 'B') {
            remove_from_level(bids_, order.price, order.order_ref_num, 0);
        } else {
            remove_from_level(asks_, order.price, order.order_ref_num, 0);
        }
        orders_.erase(it);
    }
}

// -------------------------------------------------------------------------
// cancel — a portion of the order's visible quantity was cancelled
// -------------------------------------------------------------------------
void OrderBook::cancel(const itch::ParsedOrderCancel& msg) {
    auto it = orders_.find(msg.order_ref_num);
    if (it == orders_.end()) return;

    Order& order = it->second;
    uint32_t cancelled = std::min(msg.cancelled_shares, order.shares);
    order.shares -= cancelled;

    // Update the price-level total
    if (order.side == 'B') {
        auto level_it = bids_.find(order.price);
        if (level_it != bids_.end()) {
            level_it->second.total_shares -=
                std::min(cancelled, level_it->second.total_shares);
        }
    } else {
        auto level_it = asks_.find(order.price);
        if (level_it != asks_.end()) {
            level_it->second.total_shares -=
                std::min(cancelled, level_it->second.total_shares);
        }
    }

    // If all shares were cancelled, fully remove the order
    if (order.shares == 0) {
        if (order.side == 'B') {
            remove_from_level(bids_, order.price, order.order_ref_num, 0);
        } else {
            remove_from_level(asks_, order.price, order.order_ref_num, 0);
        }
        orders_.erase(it);
    }
}

// -------------------------------------------------------------------------
// remove — the order was fully deleted from the book
// -------------------------------------------------------------------------
void OrderBook::remove(const itch::ParsedOrderDelete& msg) {
    auto it = orders_.find(msg.order_ref_num);
    if (it == orders_.end()) return;

    const Order& order = it->second;

    if (order.side == 'B') {
        remove_from_level(bids_, order.price, order.order_ref_num, order.shares);
    } else {
        remove_from_level(asks_, order.price, order.order_ref_num, order.shares);
    }

    orders_.erase(it);
}

// -------------------------------------------------------------------------
// replace — cancel original, re-add at new price/qty
//
// The ITCH spec says the replacement gets a new order_ref_num and goes to
// the back of the queue at its price level. We implement this as:
//   1. Remove the original order (simulating a delete)
//   2. Insert the new order (simulating a fresh add)
// -------------------------------------------------------------------------
void OrderBook::replace(const itch::ParsedOrderReplace& msg) {
    // Look up the original order to copy its side and stock ticker
    auto it = orders_.find(msg.orig_order_ref_num);
    if (it == orders_.end()) return;

    const Order orig = it->second; // copy before we erase

    // Step 1: remove original
    if (orig.side == 'B') {
        remove_from_level(bids_, orig.price, orig.order_ref_num, orig.shares);
    } else {
        remove_from_level(asks_, orig.price, orig.order_ref_num, orig.shares);
    }
    orders_.erase(it);

    // Step 2: insert replacement as a fresh Add Order
    itch::ParsedAddOrder replacement{};
    replacement.order_ref_num = msg.new_order_ref_num;
    replacement.side          = orig.side;
    replacement.shares        = msg.shares;
    replacement.price         = msg.price;
    replacement.stock_locate  = msg.stock_locate;
    replacement.timestamp_ns  = msg.timestamp_ns;
    replacement.has_mpid      = false;
    memcpy(replacement.stock, orig.stock, 9);

    add(replacement);
}

// -------------------------------------------------------------------------
// Accessors
// -------------------------------------------------------------------------

uint32_t OrderBook::best_bid() const {
    if (bids_.empty()) return 0;
    return bids_.begin()->first; // highest price (map sorted descending)
}

uint32_t OrderBook::best_ask() const {
    if (asks_.empty()) return 0;
    return asks_.begin()->first; // lowest price (map sorted ascending)
}

uint32_t OrderBook::spread() const {
    uint32_t bid = best_bid();
    uint32_t ask = best_ask();
    if (bid == 0 || ask == 0) return 0;
    // Guard against crossed book (bid > ask) — can happen in real ITCH data
    // during volatile periods or when the matching engine lags. Returning 0
    // signals "undefined spread" rather than wrapping a uint32_t.
    if (ask <= bid) return 0;
    return ask - bid;
}

uint32_t OrderBook::mid_price() const {
    uint32_t bid = best_bid();
    uint32_t ask = best_ask();
    if (bid == 0 || ask == 0) return 0;
    // Use the arithmetic mean even if crossed — callers should check spread()
    // first if they need a meaningful mid price.
    return (bid + ask) / 2;
}

// -------------------------------------------------------------------------
// order_imbalance_ratio — key MoE input feature
//
// Formula: OIR = (BidQty - AskQty) / (BidQty + AskQty)
//
// Positive values indicate more buy-side pressure (bullish signal).
// Negative values indicate more sell-side pressure (bearish signal).
// Uses the top `depth` price levels to focus on near-touch liquidity.
// -------------------------------------------------------------------------
double OrderBook::order_imbalance_ratio(int depth) const {
    uint64_t bid_qty = 0;
    uint64_t ask_qty = 0;

    int count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && count < depth; ++it, ++count) {
        bid_qty += it->second.total_shares;
    }

    count = 0;
    for (auto it = asks_.begin(); it != asks_.end() && count < depth; ++it, ++count) {
        ask_qty += it->second.total_shares;
    }

    uint64_t total = bid_qty + ask_qty;
    if (total == 0) return 0.0;

    return static_cast<double>(static_cast<int64_t>(bid_qty) -
                                static_cast<int64_t>(ask_qty)) /
           static_cast<double>(total);
}

// -------------------------------------------------------------------------
// top_of_book — return a snapshot of the top-N levels for display/export
// -------------------------------------------------------------------------
BookSnapshot OrderBook::top_of_book(int n) const {
    BookSnapshot snap;

    int count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && count < n; ++it, ++count) {
        snap.bids.push_back({
            it->first,
            it->second.total_shares,
            static_cast<int>(it->second.order_refs.size())
        });
    }

    count = 0;
    for (auto it = asks_.begin(); it != asks_.end() && count < n; ++it, ++count) {
        snap.asks.push_back({
            it->first,
            it->second.total_shares,
            static_cast<int>(it->second.order_refs.size())
        });
    }

    return snap;
}

// -------------------------------------------------------------------------
// print — human-readable ladder, used by --verify flag
// -------------------------------------------------------------------------
void OrderBook::print(int levels) const {
    printf("  %-12s  %-10s  %-8s  %-6s\n", "PRICE", "SHARES", "ORDERS", "SIDE");
    printf("  %-12s  %-10s  %-8s  %-6s\n",
           "------------", "----------", "--------", "------");

    BookSnapshot snap = top_of_book(levels);

    // Print asks in reverse (highest first), then bids
    for (int i = static_cast<int>(snap.asks.size()) - 1; i >= 0; --i) {
        const auto& lvl = snap.asks[i];
        printf("  %-12.4f  %-10u  %-8d  ASK\n",
               lvl.price / 10000.0, lvl.total_shares, lvl.order_count);
    }
    printf("  --- spread: $%.4f ---\n", spread() / 10000.0);
    for (const auto& lvl : snap.bids) {
        printf("  %-12.4f  %-10u  %-8d  BID\n",
               lvl.price / 10000.0, lvl.total_shares, lvl.order_count);
    }
    printf("  OIR: %+.4f  |  Mid: $%.4f  |  Orders: %zu  |  Levels: %zu\n",
           order_imbalance_ratio(), mid_price() / 10000.0,
           order_count(), level_count());
}

// -------------------------------------------------------------------------
// clear — reset all state
// -------------------------------------------------------------------------
void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    orders_.clear();
}

} // namespace book
