#pragma once
// =========================================================================
// lob.hpp — HLS Limit Order Book
//
// This module is the hardware counterpart to src/golden_model/order_book.cpp.
// It receives parsed ITCH order messages via an AXI-Stream, maintains a
// price-level book in on-chip BRAM, and outputs the updated best-bid/ask
// after every message.
//
// Key hardware architecture decisions:
//
// 1. Direct-mapped price arrays (not std::map)
//    Software uses std::map for dynamic insertion. Hardware uses flat arrays
//    indexed by price: level_idx = (price - BASE_PRICE) / TICK_SIZE.
//    With ARRAY_PARTITION, each element is a separate register → O(1) access
//    with zero memory contention, enabling II=1 pipelining.
//
// 2. Order lookup via truncated reference
//    ITCH order_ref is 64-bit. We use order_ref[11:0] (bottom 12 bits) as
//    an index into order tables (4096 entries). Hash collisions are handled
//    by checking stored_ref == incoming_ref before modifying state.
//    In production, this would use a full associative lookup or URAM.
//
// 3. Separate bid/ask arrays
//    Two independent arrays avoids read-after-write hazards when updating
//    opposite sides in the same cycle.
//
// 4. Fixed price range
//    BASE_PRICE = $100.00 × 10000 = 1,000,000
//    TICK_SIZE  = $0.01   × 10000 = 100 (1 cent resolution)
//    MAX_PRICE_LEVELS = 2048 → covers $100.00 to $120.47
//    For the demo (AAPL ~$182), shift BASE_PRICE to 1,750,000 ($175.00).
//    A production implementation would use runtime-configurable base price.
// =========================================================================

// Include real HLS headers in synthesis; stubs in g++ simulation
#ifdef __SYNTHESIS__
  #include <ap_int.h>
  #include <ap_fixed.h>
  #include <hls_stream.h>
#else
  #include "../include/hls_sim_stubs.hpp"
#endif

// -------------------------------------------------------------------------
// Book parameters — changing these changes BRAM usage and price range.
// MAX_PRICE_LEVELS must be a power of 2 for ARRAY_PARTITION.
// -------------------------------------------------------------------------
static const int MAX_PRICE_LEVELS     = 64;     // $0.01 ticks × 64 = $0.63 range
static const int MAX_ORDERS           = 4096;   // simultaneous live orders supported
static const ap_uint<32> BASE_PRICE   = 1750000; // $175.00 — center of price range
static const ap_uint<32> TICK_SIZE    = 100;     // $0.01 per tick

// -------------------------------------------------------------------------
// Message type codes (matching ITCH 5.0 spec)
// -------------------------------------------------------------------------
static const ap_uint<8> MSG_ADD     = 'A';
static const ap_uint<8> MSG_ADD_F   = 'F';
static const ap_uint<8> MSG_EXECUTE = 'E';
static const ap_uint<8> MSG_CANCEL  = 'X';
static const ap_uint<8> MSG_DELETE  = 'D';
static const ap_uint<8> MSG_REPLACE = 'U';

// -------------------------------------------------------------------------
// OrderMsg — input message struct (fed from the SV ITCH parser)
//
// The SV parser extracts these fields from the raw binary stream and packs
// them into this struct for the HLS IP. AXI-Stream carries these as a
// packed bit vector; we unpack in the HLS top function.
// -------------------------------------------------------------------------
struct OrderMsg {
    ap_uint<8>  msg_type;       // 'A', 'F', 'D', 'E', 'X', 'U'
    ap_uint<64> order_ref;      // ITCH order reference number
    ap_uint<1>  side;           // 0 = bid, 1 = ask
    ap_uint<32> shares;         // quantity (for add, cancel, replace)
    ap_uint<32> price;          // price × 10000 (for add, replace)
    ap_uint<64> new_order_ref;  // only used for Replace ('U')
    ap_uint<32> new_price;      // only used for Replace ('U')
    ap_uint<32> new_shares;     // only used for Replace ('U')
};

// -------------------------------------------------------------------------
// BookUpdate — output struct emitted after every message
//
// These fields feed directly into the MoE feature extractor.
// All prices are × 10000 (integer, no floating point in the hot path).
// -------------------------------------------------------------------------
struct BookUpdate {
    ap_uint<32> best_bid;       // highest bid price × 10000 (0 if bid side empty)
    ap_uint<32> best_ask;       // lowest ask price × 10000  (0 if ask side empty)
    ap_uint<32> spread;         // best_ask - best_bid (0 if crossed or empty)
    ap_uint<32> mid_price;      // (best_bid + best_ask) / 2
    ap_uint<32> bid_total_qty;  // total shares at best bid level
    ap_uint<32> ask_total_qty;  // total shares at best ask level
    ap_uint<8>  msg_type;       // echo input msg_type for downstream routing
};

// -------------------------------------------------------------------------
// Helper: convert a price to a price-level array index.
// Returns MAX_PRICE_LEVELS (out of range) if price is outside our window.
// -------------------------------------------------------------------------
inline ap_uint<12> price_to_idx(ap_uint<32> price) {
#pragma HLS INLINE
    if (price < BASE_PRICE) return MAX_PRICE_LEVELS;
    ap_uint<32> offset = (price - BASE_PRICE) / TICK_SIZE;
    if (offset >= MAX_PRICE_LEVELS) return MAX_PRICE_LEVELS;
    return static_cast<ap_uint<12>>(offset);
}

// -------------------------------------------------------------------------
// Helper: compute order table index from order reference number.
// We use the bottom log2(MAX_ORDERS) bits as a hash.
// In production, a content-addressable lookup would be used instead.
// -------------------------------------------------------------------------
inline ap_uint<12> order_hash(ap_uint<64> ref) {
#pragma HLS INLINE
    return static_cast<ap_uint<12>>(ref & (MAX_ORDERS - 1));
}

// -------------------------------------------------------------------------
// Top-level HLS function — called by Vitis HLS for synthesis
//
// AXI-Stream interfaces:
//   order_in  — receives OrderMsg structs from the SV parser
//   book_out  — emits BookUpdate structs to the MoE router
//
// AXI-Lite interface:
//   return (control) — start/stop/idle/ready signals
//   n_messages       — number of messages to process in this invocation
//
// II=1 goal: with ARRAY_PARTITION on bid_shares/ask_shares and the order
// tables, the synthesizer can achieve II=1 — processing one order per clock
// cycle at 250 MHz = 250 million orders/second.
// -------------------------------------------------------------------------
void process_messages(
    hls::stream<OrderMsg>&    order_in,
    hls::stream<BookUpdate>&  book_out,
    int                       n_messages
);

// Exposed for testing
BookUpdate process_one(
    const OrderMsg&     msg,
    ap_uint<32>         bid_shares[MAX_PRICE_LEVELS],
    ap_uint<32>         ask_shares[MAX_PRICE_LEVELS],
    ap_uint<64>         order_ref_table[MAX_ORDERS],
    ap_uint<1>          order_side_table[MAX_ORDERS],
    ap_uint<32>         order_price_table[MAX_ORDERS],
    ap_uint<32>         order_shares_table[MAX_ORDERS]
);
