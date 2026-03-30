#pragma once

// =========================================================================
// order_book.hpp — Price-level Limit Order Book (LOB)
//
// Design overview:
//   The LOB maintains two sides (bid and ask), each organized as a sorted
//   map from price to a PriceLevel that holds the queue of orders at that
//   price. This gives:
//     - O(log P) add/cancel/delete (P = number of distinct price levels)
//     - O(1) best-bid / best-ask (front of the sorted map)
//     - O(1) order lookup by reference number (unordered_map)
//
//   Why not a flat array / hash map for the book?
//   Because we need the sorted iteration for "top-N levels" queries and
//   order imbalance ratio. In hardware (HLS), we replace this with a
//   BRAM-partitioned fixed array indexed by price — that's where the O(1)
//   access comes from. The golden model's std::map is the ground truth that
//   the HLS implementation gets validated against.
//
//   Prices are stored as uint32_t integers (× 10000), matching the ITCH
//   wire format — no floating-point anywhere in this file.
// =========================================================================

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch_parser.hpp"

namespace book {

// -------------------------------------------------------------------------
// Order — an individual resting limit order on the book
// -------------------------------------------------------------------------
struct Order {
    uint64_t order_ref_num;
    char     side;      // 'B' = bid, 'S' = ask
    uint32_t shares;    // remaining visible quantity
    uint32_t price;     // price × 10000
    char     stock[9];  // null-terminated ticker
};

// -------------------------------------------------------------------------
// PriceLevel — all orders resting at a single price point
//
// order_refs is a FIFO deque — orders added earlier have higher priority
// within a price level (price-time priority). When an order is executed or
// cancelled it may be from any position in the queue (ITCH messages carry
// the explicit order_ref_num), so we do a linear scan through the deque to
// find and remove it. For the golden model, this is acceptable — the average
// queue depth per price level is small (typically 1–5 orders in real data).
// -------------------------------------------------------------------------
struct PriceLevel {
    uint32_t              total_shares = 0; // sum of shares across all orders here
    std::deque<uint64_t>  order_refs;       // FIFO: front = oldest, back = newest
};

// -------------------------------------------------------------------------
// BookSnapshot — top-N levels on each side, returned by top_of_book()
// -------------------------------------------------------------------------
struct LevelSnapshot {
    uint32_t price;        // price × 10000
    uint32_t total_shares; // aggregated shares at this level
    int      order_count;  // number of individual orders
};

struct BookSnapshot {
    std::vector<LevelSnapshot> bids; // sorted highest-first
    std::vector<LevelSnapshot> asks; // sorted lowest-first
};

// -------------------------------------------------------------------------
// OrderBook
// -------------------------------------------------------------------------
class OrderBook {
public:
    OrderBook() = default;

    // ------------------------------------------------------------------
    // Mutators — called from ITCHParser callbacks
    // ------------------------------------------------------------------

    // add(): insert a new order into the book.
    // Called for both 'A' (no MPID) and 'F' (with MPID) messages.
    void add(const itch::ParsedAddOrder& msg);

    // execute(): an order was matched in full or in part.
    // Reduces the order's remaining shares. If shares reach zero, the order
    // is removed from the book. Does NOT remove on partial fills.
    void execute(const itch::ParsedOrderExecuted& msg);

    // cancel(): a portion of an order's displayed quantity was cancelled.
    // Reduces shares; removes the order if shares drop to zero.
    void cancel(const itch::ParsedOrderCancel& msg);

    // remove(): the order was fully deleted from the book.
    void remove(const itch::ParsedOrderDelete& msg);

    // replace(): cancel the original order and re-add at new price/qty.
    // The replacement goes to the back of the queue (loses time priority).
    void replace(const itch::ParsedOrderReplace& msg);

    // ------------------------------------------------------------------
    // Read-only accessors — called by MoE feature extractor
    // ------------------------------------------------------------------

    // Returns best bid price (× 10000). Returns 0 if bid side is empty.
    uint32_t best_bid() const;

    // Returns best ask price (× 10000). Returns 0 if ask side is empty.
    uint32_t best_ask() const;

    // Returns spread = best_ask - best_bid (× 10000). Returns 0 if either
    // side is empty.
    uint32_t spread() const;

    // Returns mid price = (best_bid + best_ask) / 2 (× 10000).
    // Returns 0 if either side is empty.
    uint32_t mid_price() const;

    // Order imbalance ratio = (bid_qty - ask_qty) / (bid_qty + ask_qty).
    // Returns a value in [-1.0, 1.0]. Positive = bid-heavy (buy pressure).
    // Returns 0.0 if the book is empty.
    // Uses the top-N levels on each side (default N=5) to avoid being
    // dominated by deep, stale liquidity.
    double order_imbalance_ratio(int depth = 5) const;

    // Returns a snapshot of the top-N price levels on each side.
    BookSnapshot top_of_book(int n = 5) const;

    // Returns the number of active orders currently in the book.
    size_t order_count() const { return orders_.size(); }

    // Returns total number of distinct price levels (bid + ask).
    size_t level_count() const { return bids_.size() + asks_.size(); }

    // Print a human-readable ladder to stdout (for --verify output)
    void print(int levels = 5) const;

    // Reset all state (used between test cases)
    void clear();

private:
    // Bid side: highest price first — std::greater<uint32_t> reverses order
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;
    // Ask side: lowest price first — default std::less<uint32_t>
    std::map<uint32_t, PriceLevel> asks_;

    // Primary order lookup — O(1) average access by order reference number.
    // We need this to find an order's price+side when processing E/X/D/U
    // messages, which only carry the order_ref_num.
    std::unordered_map<uint64_t, Order> orders_;

    // Internal helpers
    // Remove an order from the appropriate bid/ask level map.
    // If the level becomes empty after removal, erase the level entry.
    // Templated on map type so it works for both bid (greater) and ask (less).
    template<typename Map>
    void remove_from_level(Map& side_map, uint32_t price, uint64_t order_ref_num, uint32_t shares);
};

} // namespace book
