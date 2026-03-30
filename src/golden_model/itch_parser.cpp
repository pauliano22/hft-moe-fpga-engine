// =========================================================================
// itch_parser.cpp — NASDAQ TotalView-ITCH 5.0 binary stream parser
// See itch_parser.hpp for the design overview.
// =========================================================================

#include "itch_parser.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace itch {

// -------------------------------------------------------------------------
// parse_file — load the entire file into a heap buffer, then iterate through
// length-prefixed messages.
//
// Why read the whole file at once?
//   A full NASDAQ trading day is ~5 GB. Reading message-by-message with
//   fread() per message would issue ~60 million syscalls — catastrophic for
//   throughput. Loading the whole file (or a large chunk) into memory and
//   iterating over it in userspace reduces syscall overhead to one fread().
//   For the benchmark, the file stays hot in the page cache after the first
//   pass.
//
//   On machines with <8 GB RAM, a real trading-day file won't fit. For the
//   synthetic sample (~tens of MB) this is never a concern.
// -------------------------------------------------------------------------
bool ITCHParser::parse_file(const std::string& path, ParseStats& stats) {
    // Open in binary mode — critical on Windows where text mode translates
    // \r\n, corrupting the binary data.
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }

    // Determine file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return false;
    }

    // Allocate and read entire file
    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    size_t bytes_read = fread(buf.data(), 1, static_cast<size_t>(file_size), f);
    fclose(f);

    if (bytes_read != static_cast<size_t>(file_size)) {
        return false;
    }

    stats.bytes_read += bytes_read;

    // Walk the buffer message by message.
    // Format: [uint16_t length (big-endian)] [message body of `length` bytes]
    const uint8_t* ptr = buf.data();
    const uint8_t* end = buf.data() + bytes_read;

    while (ptr + 2 <= end) {
        // Read 2-byte big-endian length prefix
        uint16_t msg_len = (static_cast<uint16_t>(ptr[0]) << 8) |
                            static_cast<uint16_t>(ptr[1]);
        ptr += 2;

        // Validate that the declared length doesn't run past end of file
        if (ptr + msg_len > end) {
            stats.parse_errors++;
            break;  // truncated file — stop here rather than read garbage
        }

        parse_message(ptr, msg_len, stats);
        ptr += msg_len;
    }

    return true;
}

// -------------------------------------------------------------------------
// parse_message — dispatch a single raw message to the appropriate handler
// -------------------------------------------------------------------------
bool ITCHParser::parse_message(const uint8_t* buf, uint16_t len, ParseStats& stats) {
    if (len == 0) {
        stats.parse_errors++;
        return false;
    }

    stats.total_messages++;
    char msg_type = static_cast<char>(buf[0]);
    stats.msg_type_counts[msg_type]++;

    switch (msg_type) {
        case 'S': dispatch_system_event  (buf, len);       return true;
        case 'A': dispatch_add_order     (buf, len, false); return true;
        case 'F': dispatch_add_order     (buf, len, true);  return true;
        case 'E': dispatch_order_executed(buf, len);       return true;
        case 'X': dispatch_order_cancel  (buf, len);       return true;
        case 'D': dispatch_order_delete  (buf, len);       return true;
        case 'U': dispatch_order_replace (buf, len);       return true;
        case 'P': dispatch_trade         (buf, len);       return true;
        default:
            // Silently skip unrecognised types — real ITCH files contain
            // many message types (R, H, Y, L, V, W, K, J, C, Q, B, I, N)
            // that we don't need for the order book. This is not an error.
            return false;
    }
}

// -------------------------------------------------------------------------
// msg_type_name — human-readable label for a message type byte
// -------------------------------------------------------------------------
const char* ITCHParser::msg_type_name(char t) {
    switch (t) {
        case 'S': return "SystemEvent";
        case 'A': return "AddOrder";
        case 'F': return "AddOrderMPID";
        case 'E': return "OrderExecuted";
        case 'X': return "OrderCancel";
        case 'D': return "OrderDelete";
        case 'U': return "OrderReplace";
        case 'P': return "Trade";
        case 'R': return "StockDirectory";
        case 'H': return "StockTradingAction";
        case 'Y': return "RegSHORestriction";
        case 'L': return "MarketParticipantPos";
        case 'V': return "MWCBDeclineLevel";
        case 'W': return "MWCBStatus";
        case 'K': return "IPOQuotingPeriod";
        case 'J': return "LULDAuctionCollar";
        case 'C': return "OrderExecutedWithPrice";
        case 'Q': return "CrossTrade";
        case 'B': return "BrokenTrade";
        case 'I': return "NOIIMessage";
        case 'N': return "RPIIMessage";
        default:  return "Unknown";
    }
}

// =========================================================================
// Dispatch helpers
//
// Each function:
//   1. Validates the minimum message length before dereferencing any fields
//   2. Casts the raw buffer to the packed wire struct
//   3. Converts each multi-byte field from big-endian to host order
//   4. Populates the host-endian Parsed* struct
//   5. Calls the registered callback (if set)
// =========================================================================

void ITCHParser::dispatch_system_event(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(SystemEventMsg)) return;
    if (!on_system_event_) return;

    const auto* w = reinterpret_cast<const SystemEventMsg*>(buf);
    ParsedSystemEvent out;
    out.stock_locate  = be16(w->header.stock_locate);
    out.timestamp_ns  = be48(w->header.timestamp);
    out.event_code    = static_cast<char>(w->event_code);
    on_system_event_(out);
}

void ITCHParser::dispatch_add_order(const uint8_t* buf, uint16_t len, bool has_mpid) {
    // Both 'A' and 'F' share the first 36 bytes; 'F' adds 4 more for attribution
    if (len < sizeof(AddOrderMsg)) return;
    if (!on_add_order_) return;

    const auto* w = reinterpret_cast<const AddOrderMsg*>(buf);
    ParsedAddOrder out;
    out.stock_locate  = be16(w->header.stock_locate);
    out.timestamp_ns  = be48(w->header.timestamp);
    out.order_ref_num = be64(w->order_ref_num);
    out.side          = static_cast<char>(w->side);
    out.shares        = be32(w->shares);
    out.price         = be32(w->price);
    out.has_mpid      = has_mpid;

    // Copy and null-terminate the 8-byte stock field
    memcpy(out.stock, w->stock, 8);
    out.stock[8] = '\0';

    if (has_mpid && len >= sizeof(AddOrderMPIDMsg)) {
        const auto* wf = reinterpret_cast<const AddOrderMPIDMsg*>(buf);
        memcpy(out.mpid, wf->attribution, 4);
        out.mpid[4] = '\0';
    } else {
        out.mpid[0] = '\0';
    }

    on_add_order_(out);
}

void ITCHParser::dispatch_order_executed(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(OrderExecutedMsg)) return;
    if (!on_order_executed_) return;

    const auto* w = reinterpret_cast<const OrderExecutedMsg*>(buf);
    ParsedOrderExecuted out;
    out.stock_locate     = be16(w->header.stock_locate);
    out.timestamp_ns     = be48(w->header.timestamp);
    out.order_ref_num    = be64(w->order_ref_num);
    out.executed_shares  = be32(w->executed_shares);
    out.match_number     = be64(w->match_number);
    on_order_executed_(out);
}

void ITCHParser::dispatch_order_cancel(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(OrderCancelMsg)) return;
    if (!on_order_cancel_) return;

    const auto* w = reinterpret_cast<const OrderCancelMsg*>(buf);
    ParsedOrderCancel out;
    out.stock_locate      = be16(w->header.stock_locate);
    out.timestamp_ns      = be48(w->header.timestamp);
    out.order_ref_num     = be64(w->order_ref_num);
    out.cancelled_shares  = be32(w->cancelled_shares);
    on_order_cancel_(out);
}

void ITCHParser::dispatch_order_delete(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(OrderDeleteMsg)) return;
    if (!on_order_delete_) return;

    const auto* w = reinterpret_cast<const OrderDeleteMsg*>(buf);
    ParsedOrderDelete out;
    out.stock_locate  = be16(w->header.stock_locate);
    out.timestamp_ns  = be48(w->header.timestamp);
    out.order_ref_num = be64(w->order_ref_num);
    on_order_delete_(out);
}

void ITCHParser::dispatch_order_replace(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(OrderReplaceMsg)) return;
    if (!on_order_replace_) return;

    const auto* w = reinterpret_cast<const OrderReplaceMsg*>(buf);
    ParsedOrderReplace out;
    out.stock_locate       = be16(w->header.stock_locate);
    out.timestamp_ns       = be48(w->header.timestamp);
    out.orig_order_ref_num = be64(w->orig_order_ref_num);
    out.new_order_ref_num  = be64(w->new_order_ref_num);
    out.shares             = be32(w->shares);
    out.price              = be32(w->price);
    on_order_replace_(out);
}

void ITCHParser::dispatch_trade(const uint8_t* buf, uint16_t len) {
    if (len < sizeof(TradeMsg)) return;
    if (!on_trade_) return;

    const auto* w = reinterpret_cast<const TradeMsg*>(buf);
    ParsedTrade out;
    out.stock_locate  = be16(w->header.stock_locate);
    out.timestamp_ns  = be48(w->header.timestamp);
    out.order_ref_num = be64(w->order_ref_num);
    out.side          = static_cast<char>(w->side);
    out.shares        = be32(w->shares);
    out.price         = be32(w->price);
    out.match_number  = be64(w->match_number);
    memcpy(out.stock, w->stock, 8);
    out.stock[8] = '\0';
    on_trade_(out);
}

} // namespace itch
