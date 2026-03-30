#pragma once

// =========================================================================
// itch_parser.hpp — NASDAQ TotalView-ITCH 5.0 binary stream parser
//
// Design overview:
//   1. Wire-format structs (pragma-packed) for direct memory mapping of raw
//      bytes. These are NEVER used directly by application code — all
//      numeric fields are still big-endian inside them.
//   2. Parsed structs (host byte order) that callers actually work with.
//   3. ITCHParser class that reads a length-prefixed binary stream, converts
//      endianness, and dispatches via std::function callbacks.
//
// Why callbacks instead of virtual dispatch?
//   The order book registers exactly the message types it cares about.
//   Unregistered types are skipped with zero overhead. This matches the
//   hardware design, where different pipeline stages subscribe to different
//   event streams.
//
// Reference: NASDAQ TotalView-ITCH 5.0 specification
// =========================================================================

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>

namespace itch {

// -------------------------------------------------------------------------
// Portable endian conversion helpers
//
// ITCH 5.0 is big-endian throughout. On x86/x86-64 (little-endian) hosts,
// every multi-byte field read from raw bytes must be byte-swapped.
// We use __builtin_bswap* (GCC/Clang) rather than htonl/ntohl to avoid
// pulling in platform-specific network headers.
// -------------------------------------------------------------------------

inline uint16_t be16(uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(v);
#else
    return v;
#endif
}

inline uint32_t be32(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}

inline uint64_t be64(uint64_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

// Extract a 6-byte (48-bit) big-endian timestamp into a uint64_t.
// ITCH timestamps represent nanoseconds since midnight — they exceed 32 bits
// by late in the trading day (~34 billion ns), so we always use 64 bits.
inline uint64_t be48(const uint8_t* p) {
    return ((uint64_t)p[0] << 40) |
           ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) |
           ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] <<  8) |
           ((uint64_t)p[5]);
}

// -------------------------------------------------------------------------
// Wire-format structs
//
// #pragma pack(push, 1) disables all struct padding so sizeof() exactly
// matches the on-wire byte count. This lets us cast a raw byte pointer
// directly to these structs for field extraction.
//
// IMPORTANT: never read a uint16_t/uint32_t/uint64_t field directly from
// these structs without calling the matching be*() converter — on a
// little-endian host the raw value will be wrong.
// -------------------------------------------------------------------------

#pragma pack(push, 1)

// Common header shared by every ITCH message (11 bytes)
struct MsgHeader {
    uint8_t  msg_type;          // Message type character ('S', 'A', 'F', etc.)
    uint16_t stock_locate;      // NASDAQ-assigned stock identifier (big-endian)
    uint16_t tracking_number;   // NASDAQ internal sequence number (big-endian)
    uint8_t  timestamp[6];      // Nanoseconds since midnight, 48-bit big-endian
};
static_assert(sizeof(MsgHeader) == 11, "MsgHeader packing error");

// System Event ('S') — 12 bytes
// Marks trading-day milestones: session open/close, halt start/end, etc.
struct SystemEventMsg {
    MsgHeader header;
    uint8_t   event_code;   // 'O'=start hours, 'S'=start system, 'Q'=start market,
                            // 'M'=end market, 'E'=end system, 'C'=end hours
};
static_assert(sizeof(SystemEventMsg) == 12, "SystemEventMsg packing error");

// Add Order — no MPID attribution ('A') — 36 bytes
// A new limit order entered the visible book.
struct AddOrderMsg {
    MsgHeader header;
    uint64_t  order_ref_num;    // Unique order identifier for this session (big-endian)
    uint8_t   side;             // 'B' = buy, 'S' = sell
    uint32_t  shares;           // Visible quantity (big-endian)
    char      stock[8];         // Ticker, space-padded (e.g. "AAPL    ")
    uint32_t  price;            // Price × 10000, e.g. $10.25 stored as 102500 (big-endian)
};
static_assert(sizeof(AddOrderMsg) == 36, "AddOrderMsg packing error");

// Add Order with MPID ('F') — 40 bytes
// Same as Add Order but includes a 4-byte market participant attribution.
struct AddOrderMPIDMsg {
    MsgHeader header;
    uint64_t  order_ref_num;
    uint8_t   side;
    uint32_t  shares;
    char      stock[8];
    uint32_t  price;
    char      attribution[4];   // Market participant ID (MPID), e.g. "GSCO"
};
static_assert(sizeof(AddOrderMPIDMsg) == 40, "AddOrderMPIDMsg packing error");

// Order Executed ('E') — 31 bytes
// An order in the book was executed in full or in part at its limit price.
struct OrderExecutedMsg {
    MsgHeader header;
    uint64_t  order_ref_num;    // Order that was (partially) executed
    uint32_t  executed_shares;  // Number of shares executed in this event
    uint64_t  match_number;     // Cross/match event identifier (big-endian)
};
static_assert(sizeof(OrderExecutedMsg) == 31, "OrderExecutedMsg packing error");

// Order Cancel ('X') — 23 bytes
// A portion of an order's displayed quantity was cancelled.
// The order remains live at (original_shares - cancelled_shares).
struct OrderCancelMsg {
    MsgHeader header;
    uint64_t  order_ref_num;
    uint32_t  cancelled_shares; // Shares REMOVED — decrement remaining quantity
};
static_assert(sizeof(OrderCancelMsg) == 23, "OrderCancelMsg packing error");

// Order Delete ('D') — 19 bytes
// An order was fully removed from the book.
struct OrderDeleteMsg {
    MsgHeader header;
    uint64_t  order_ref_num;
};
static_assert(sizeof(OrderDeleteMsg) == 19, "OrderDeleteMsg packing error");

// Order Replace ('U') — 35 bytes
// An order was cancelled and re-entered at a new price/quantity.
// The original order_ref_num is retired; a new one is issued.
// Note: replace always loses time priority — the new order goes to the back
// of the queue at its price level.
struct OrderReplaceMsg {
    MsgHeader header;
    uint64_t  orig_order_ref_num;   // Reference of the order being replaced
    uint64_t  new_order_ref_num;    // Reference of the replacement order
    uint32_t  shares;               // New quantity
    uint32_t  price;                // New price × 10000
};
static_assert(sizeof(OrderReplaceMsg) == 35, "OrderReplaceMsg packing error");

// Non-Cross Trade ('P') — 44 bytes
// A trade that is not associated with a displayed book order
// (e.g. hidden orders, crosses). Does not affect the visible book.
struct TradeMsg {
    MsgHeader header;
    uint64_t  order_ref_num;
    uint8_t   side;
    uint32_t  shares;
    char      stock[8];
    uint32_t  price;
    uint64_t  match_number;
};
static_assert(sizeof(TradeMsg) == 44, "TradeMsg packing error");

#pragma pack(pop)

// -------------------------------------------------------------------------
// Parsed (host-endian) message structs
//
// These are what callers receive in callbacks. All integers are in native
// byte order. Strings are null-terminated. Prices are still × 10000 to
// avoid floating-point — convert with price / 10000.0 only when displaying.
// -------------------------------------------------------------------------

struct ParsedSystemEvent {
    uint16_t stock_locate;
    uint64_t timestamp_ns;  // nanoseconds since midnight
    char     event_code;
};

struct ParsedAddOrder {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    char     side;          // 'B' or 'S'
    uint32_t shares;
    char     stock[9];      // null-terminated (8 wire chars + '\0')
    uint32_t price;         // price × 10000 in host byte order
    bool     has_mpid;
    char     mpid[5];       // null-terminated, valid only if has_mpid
};

struct ParsedOrderExecuted {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct ParsedOrderCancel {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    uint32_t cancelled_shares;
};

struct ParsedOrderDelete {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
};

struct ParsedOrderReplace {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t orig_order_ref_num;
    uint64_t new_order_ref_num;
    uint32_t shares;
    uint32_t price;
};

struct ParsedTrade {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    char     side;
    uint32_t shares;
    char     stock[9];
    uint32_t price;
    uint64_t match_number;
};

// -------------------------------------------------------------------------
// Parse statistics — accumulated across a parse_file() call
// -------------------------------------------------------------------------
struct ParseStats {
    uint64_t total_messages = 0;
    uint64_t parse_errors   = 0;
    uint64_t bytes_read     = 0;
    std::unordered_map<char, uint64_t> msg_type_counts;

    void reset() {
        total_messages = 0;
        parse_errors   = 0;
        bytes_read     = 0;
        msg_type_counts.clear();
    }
};

// -------------------------------------------------------------------------
// Callback type aliases — register only the types your subscriber needs
// -------------------------------------------------------------------------
using OnSystemEvent   = std::function<void(const ParsedSystemEvent&)>;
using OnAddOrder      = std::function<void(const ParsedAddOrder&)>;
using OnOrderExecuted = std::function<void(const ParsedOrderExecuted&)>;
using OnOrderCancel   = std::function<void(const ParsedOrderCancel&)>;
using OnOrderDelete   = std::function<void(const ParsedOrderDelete&)>;
using OnOrderReplace  = std::function<void(const ParsedOrderReplace&)>;
using OnTrade         = std::function<void(const ParsedTrade&)>;

// -------------------------------------------------------------------------
// ITCHParser
//
// Usage:
//   ITCHParser parser;
//   parser.set_on_add_order([&](const ParsedAddOrder& msg) { book.add(msg); });
//   parser.set_on_order_delete([&](const ParsedOrderDelete& msg) { book.remove(msg); });
//   ParseStats stats;
//   parser.parse_file("data/sample.itch", stats);
// -------------------------------------------------------------------------
class ITCHParser {
public:
    ITCHParser() = default;

    // Register callbacks — passing nullptr (or not calling set_*) skips the type
    void set_on_system_event  (OnSystemEvent   cb) { on_system_event_   = cb; }
    void set_on_add_order     (OnAddOrder      cb) { on_add_order_      = cb; }
    void set_on_order_executed(OnOrderExecuted cb) { on_order_executed_ = cb; }
    void set_on_order_cancel  (OnOrderCancel   cb) { on_order_cancel_   = cb; }
    void set_on_order_delete  (OnOrderDelete   cb) { on_order_delete_   = cb; }
    void set_on_order_replace (OnOrderReplace  cb) { on_order_replace_  = cb; }
    void set_on_trade         (OnTrade         cb) { on_trade_          = cb; }

    // Parse an entire binary ITCH file. Reads the whole file into memory for
    // maximum throughput (avoids per-message syscalls). Returns false only on
    // fatal I/O errors; parse errors are counted in stats.parse_errors.
    bool parse_file(const std::string& path, ParseStats& stats);

    // Parse a single message from a raw buffer (length prefix already consumed).
    // len = the value from the 2-byte length prefix (body length only).
    // Returns true if the message type was recognized.
    bool parse_message(const uint8_t* buf, uint16_t len, ParseStats& stats);

    // Return a human-readable name for a message type byte (for reporting)
    static const char* msg_type_name(char t);

private:
    OnSystemEvent   on_system_event_;
    OnAddOrder      on_add_order_;
    OnOrderExecuted on_order_executed_;
    OnOrderCancel   on_order_cancel_;
    OnOrderDelete   on_order_delete_;
    OnOrderReplace  on_order_replace_;
    OnTrade         on_trade_;

    // One dispatch function per message type — validates size, extracts and
    // converts fields, then invokes the registered callback if set.
    void dispatch_system_event  (const uint8_t* buf, uint16_t len);
    void dispatch_add_order     (const uint8_t* buf, uint16_t len, bool has_mpid);
    void dispatch_order_executed(const uint8_t* buf, uint16_t len);
    void dispatch_order_cancel  (const uint8_t* buf, uint16_t len);
    void dispatch_order_delete  (const uint8_t* buf, uint16_t len);
    void dispatch_order_replace (const uint8_t* buf, uint16_t len);
    void dispatch_trade         (const uint8_t* buf, uint16_t len);
};

} // namespace itch
