// =============================================================================
// ITCH 5.0 Line-Rate Parser ("The Shell")
// =============================================================================
//
// WHAT THIS MODULE DOES:
//   Processes raw bytes from a 10 Gigabit Ethernet MAC (arriving 8 bytes at a
//   time on a 64-bit AXI-Stream bus) and extracts "Add Order" messages (Type
//   'A' = 0x41) byte-by-byte. When a complete Add Order is received, it outputs
//   all the parsed fields (timestamp, order ref, side, shares, stock, price)
//   for exactly one clock cycle.
//
// HOW IT WORKS:
//   A Finite State Machine (FSM) counts bytes as they arrive. On each clock
//   cycle, one AXI-Stream "beat" brings 8 new bytes. The FSM knows which bytes
//   in each beat correspond to which fields (based on the ITCH spec offsets)
//   and extracts them into accumulation registers. After 5 beats (40 bytes ≥
//   36-byte message), all fields are ready and `order_valid` is asserted.
//
// WHY "LINE-RATE"?
//   The parser never creates backpressure — it always accepts data (tready=1).
//   This is critical: in trading, dropping even one packet means missing an
//   order. The FSM is designed to keep up with the full 10 Gbps wire speed.
//
// ITCH Add Order Message Layout (36 bytes):
//   Offset  Size  Field                    Beat   Byte within beat
//   ------  ----  -----                    ----   ----------------
//   0       1     Message Type ('A')       0      0
//   1       2     Stock Locate             0      1-2
//   3       2     Tracking Number          0      3-4
//   5       6     Timestamp (ns)           0-1    5-7, then 0-2
//   11      8     Order Reference Number   1-2    3-7, then 0-2
//   19      1     Buy/Sell Indicator       2      3
//   20      4     Shares                   2      4-7
//   24      8     Stock (ASCII)            3      0-7
//   32      4     Price                    4      0-3
//
// DESIGN PATTERN:
//   This uses the classic "two-process FSM" style in SystemVerilog:
//   - always_ff: Sequential block — updates registers on clock edge
//   - always_comb: Combinational block — computes next state from current state
//   This separation makes it easy to reason about what's stored vs. computed.

module itch_parser
    import axi_stream_pkg::*;  // Import all types from our package
(
    // -------------------------------------------------------------------------
    // Clock and Reset
    // -------------------------------------------------------------------------
    input  logic                        clk,       // System clock (e.g., 250 MHz)
    input  logic                        rst_n,     // Active-low reset (0 = reset)

    // -------------------------------------------------------------------------
    // AXI-Stream Slave Interface (from 10GbE MAC)
    // -------------------------------------------------------------------------
    // "Slave" means we RECEIVE data. The MAC is the "Master" that sends it.
    input  logic [AXIS_DATA_WIDTH-1:0]  s_axis_tdata,   // 64 bits of data per beat
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [AXIS_KEEP_WIDTH-1:0]  s_axis_tkeep,   // Byte enables (not used — we always assume 8 valid bytes)
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                        s_axis_tvalid,   // MAC says "data on the bus is valid"
    input  logic                        s_axis_tlast,    // MAC says "this is the last beat of the packet"
    output logic                        s_axis_tready,   // We say "ready to accept" (always 1)

    // -------------------------------------------------------------------------
    // Parsed Output
    // -------------------------------------------------------------------------
    // When order_out.valid == 1, all fields contain a freshly parsed order.
    // Valid is high for exactly ONE clock cycle, then returns to 0.
    output parsed_add_order_t           order_out,

    // -------------------------------------------------------------------------
    // Statistics Counters
    // -------------------------------------------------------------------------
    // These count up over time. Useful for debugging and performance monitoring.
    output logic [63:0]                 msg_count,       // Total messages seen (all types)
    output logic [63:0]                 add_order_count  // Only Add Order messages parsed
);

    // -------------------------------------------------------------------------
    // FSM States
    // -------------------------------------------------------------------------
    // The FSM walks through these states as it processes each Add Order:
    //   IDLE → HEADER → ORDER_REF → STOCK → PRICE → (emit) → IDLE
    // Non-Add-Order messages: IDLE → SKIP → (wait for TLAST) → IDLE
    typedef enum logic [2:0] {
        S_IDLE,         // Waiting for message type byte (byte 0 of packet)
        S_HEADER,       // Consuming header: remaining timestamp + start of order ref
        S_ORDER_REF,    // Consuming: rest of order ref + side + shares
        S_SIDE_SHARES,  // (Folded into S_ORDER_REF — kept for readability)
        S_STOCK,        // Consuming 8-byte stock symbol
        S_PRICE,        // Consuming 4-byte price → then EMIT order
        S_SKIP          // Skipping a non-Add-Order message until TLAST
    } state_t;

    state_t state, state_next;

    // -------------------------------------------------------------------------
    // Internal Registers
    // -------------------------------------------------------------------------
    // These hold the current byte position and partially-accumulated fields.
    // "Accumulation registers" build up multi-byte fields across beats.
    logic [5:0]  byte_counter;      // Tracks position within 36-byte message
    logic [5:0]  byte_counter_next;

    // Accumulation registers — these build up field values as bytes arrive.
    // For example, the 6-byte timestamp arrives across 2 beats, so we store
    // the first 3 bytes, then OR in the remaining 3 bytes next cycle.
    logic [47:0] timestamp_acc;     // 6-byte timestamp (nanoseconds since midnight)
    logic [63:0] order_ref_acc;     // 8-byte unique order identifier
    logic [31:0] shares_acc;        // 4-byte share count
    logic [63:0] stock_acc;         // 8-byte ASCII stock symbol
    logic [31:0] price_acc;         // 4-byte price (× 10000)
    logic        side_acc;          // 1 bit: 0=Buy, 1=Sell

    // -------------------------------------------------------------------------
    // Backpressure-Free Operation
    // -------------------------------------------------------------------------
    // We ALWAYS accept data. In a trading system, creating backpressure
    // (tready=0) would cause the MAC to buffer or drop packets — unacceptable.
    // Our FSM is guaranteed to keep up because it processes 8 bytes per cycle.
    assign s_axis_tready = 1'b1;

    // -------------------------------------------------------------------------
    // Byte Extraction Helper
    // -------------------------------------------------------------------------
    // Given a 64-bit AXI-Stream beat, extract byte number `idx` (0-7).
    //
    // ITCH is big-endian: the most significant byte comes first.
    // AXI-Stream convention: byte 0 is in tdata[63:56] (MSB).
    //
    // So to get byte idx from the 64-bit word:
    //   byte 0 → bits [63:56] → offset = (7-0)*8 = 56
    //   byte 1 → bits [55:48] → offset = (7-1)*8 = 48
    //   byte 7 → bits [7:0]   → offset = (7-7)*8 = 0
    function automatic logic [7:0] get_byte(
        input logic [63:0] data,
        input int unsigned idx  // 0..7 within the beat
    );
        return data[(7 - idx) * 8 +: 8];
    endfunction

    // -------------------------------------------------------------------------
    // FSM: Sequential Logic (Registers — updated on clock edge)
    // -------------------------------------------------------------------------
    // This block defines what happens on each rising clock edge:
    //   - On reset: clear everything
    //   - On each valid beat: update accumulators based on current state
    //   - On S_PRICE: emit the complete parsed order
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            // Asynchronous reset: zero everything
            state        <= S_IDLE;
            byte_counter <= '0;
            msg_count    <= '0;
            add_order_count <= '0;
            order_out.valid <= 1'b0;

            timestamp_acc <= '0;
            order_ref_acc <= '0;
            shares_acc    <= '0;
            stock_acc     <= '0;
            price_acc     <= '0;
            side_acc      <= '0;
        end else begin
            // Default: deassert valid after one cycle (pulse behavior)
            order_out.valid <= 1'b0;

            // Only process when the MAC is sending valid data
            if (s_axis_tvalid && s_axis_tready) begin
                state        <= state_next;
                byte_counter <= byte_counter_next;

                case (state)
                    // ---------------------------------------------------------
                    // IDLE: First beat of a new message
                    // ---------------------------------------------------------
                    // Beat 0 contains: [MsgType(1), Locate(2), Tracking(2), Timestamp(3)]
                    // That's bytes 0-7 of the ITCH message.
                    S_IDLE: begin
                        if (get_byte(s_axis_tdata, 0) == ITCH_ADD_ORDER) begin
                            // It's an Add Order! Start accumulating fields.
                            msg_count <= msg_count + 1;

                            // Timestamp occupies bytes 5-10 of the message.
                            // In beat 0, bytes 5-7 are at AXI positions 5-7.
                            // That's the upper 3 bytes of the 6-byte timestamp.
                            timestamp_acc[47:24] <= {get_byte(s_axis_tdata, 5),
                                                     get_byte(s_axis_tdata, 6),
                                                     get_byte(s_axis_tdata, 7)};
                        end else begin
                            // Not an Add Order — still count it, but skip
                            msg_count <= msg_count + 1;
                        end
                    end

                    // ---------------------------------------------------------
                    // HEADER: Second beat (bytes 8-15 of message)
                    // ---------------------------------------------------------
                    // Contains: [Timestamp(3), OrderRef(5)]
                    // Timestamp bytes 8-10 (message offset) → beat positions 0-2
                    // OrderRef bytes 11-15 (message offset) → beat positions 3-7
                    S_HEADER: begin
                        // Lower 3 bytes of the 6-byte timestamp
                        timestamp_acc[23:0] <= {get_byte(s_axis_tdata, 0),
                                                get_byte(s_axis_tdata, 1),
                                                get_byte(s_axis_tdata, 2)};
                        // Upper 5 bytes of the 8-byte order reference
                        order_ref_acc[63:24] <= {get_byte(s_axis_tdata, 3),
                                                 get_byte(s_axis_tdata, 4),
                                                 get_byte(s_axis_tdata, 5),
                                                 get_byte(s_axis_tdata, 6),
                                                 get_byte(s_axis_tdata, 7)};
                    end

                    // ---------------------------------------------------------
                    // ORDER_REF: Third beat (bytes 16-23 of message)
                    // ---------------------------------------------------------
                    // Contains: [OrderRef(3), Side(1), Shares(4)]
                    S_ORDER_REF: begin
                        // Lower 3 bytes of order reference
                        order_ref_acc[23:0] <= {get_byte(s_axis_tdata, 0),
                                                get_byte(s_axis_tdata, 1),
                                                get_byte(s_axis_tdata, 2)};
                        // Side indicator: 'B' (0x42) = Buy, 'S' (0x53) = Sell
                        // We store 1 for Sell, 0 for Buy
                        side_acc <= (get_byte(s_axis_tdata, 3) == 8'h53); // 'S' = Sell
                        // 4-byte shares field
                        shares_acc <= {get_byte(s_axis_tdata, 4),
                                       get_byte(s_axis_tdata, 5),
                                       get_byte(s_axis_tdata, 6),
                                       get_byte(s_axis_tdata, 7)};
                    end

                    // ---------------------------------------------------------
                    // STOCK: Fourth beat (bytes 24-31 of message)
                    // ---------------------------------------------------------
                    // Contains: [Stock Symbol (8 bytes, right-padded with spaces)]
                    // Example: "AAPL    " = {0x41,0x41,0x50,0x4C,0x20,0x20,0x20,0x20}
                    S_STOCK: begin
                        stock_acc <= {get_byte(s_axis_tdata, 0),
                                      get_byte(s_axis_tdata, 1),
                                      get_byte(s_axis_tdata, 2),
                                      get_byte(s_axis_tdata, 3),
                                      get_byte(s_axis_tdata, 4),
                                      get_byte(s_axis_tdata, 5),
                                      get_byte(s_axis_tdata, 6),
                                      get_byte(s_axis_tdata, 7)};
                    end

                    // ---------------------------------------------------------
                    // PRICE: Fifth beat (bytes 32-35 of message)
                    // ---------------------------------------------------------
                    // Contains: [Price (4 bytes)] — only first 4 bytes of beat are valid
                    // Price has 4 implied decimal places: $150.25 → 1502500
                    //
                    // THIS IS WHERE WE EMIT THE PARSED ORDER.
                    // After extracting the price, we have all fields — assert valid.
                    S_PRICE: begin
                        price_acc <= {get_byte(s_axis_tdata, 0),
                                      get_byte(s_axis_tdata, 1),
                                      get_byte(s_axis_tdata, 2),
                                      get_byte(s_axis_tdata, 3)};

                        // === EMIT: All fields are now ready ===
                        order_out.valid     <= 1'b1;  // Pulse high for 1 cycle
                        order_out.order_ref <= order_ref_acc;
                        order_out.side      <= side_acc;
                        order_out.shares    <= shares_acc;
                        order_out.stock     <= stock_acc;
                        order_out.price     <= price_acc;
                        order_out.timestamp <= timestamp_acc;

                        add_order_count <= add_order_count + 1;
                    end

                    default: ;  // S_SKIP: do nothing, just wait for TLAST
                endcase
            end
        end
    end

    // -------------------------------------------------------------------------
    // FSM: Combinational Next-State Logic (Wires — computed instantly)
    // -------------------------------------------------------------------------
    // This block computes what the NEXT state should be based on the CURRENT
    // state and inputs. It doesn't create any registers — it's pure logic.
    //
    // Separating sequential (above) from combinational (here) is a best
    // practice in RTL design. It prevents accidental latches and makes the
    // design easier to verify.
    always_comb begin
        // Default: stay in current state
        state_next        = state;
        byte_counter_next = byte_counter;

        case (state)
            S_IDLE: begin
                if (s_axis_tvalid) begin
                    if (get_byte(s_axis_tdata, 0) == ITCH_ADD_ORDER) begin
                        // Add Order detected → start parsing
                        state_next        = S_HEADER;
                        byte_counter_next = 6'd8;  // We've consumed 8 bytes (beat 0)
                    end else begin
                        // Not an Add Order → skip until end of packet
                        state_next = S_SKIP;
                    end
                end
            end

            S_HEADER: begin
                if (s_axis_tvalid) begin
                    state_next        = S_ORDER_REF;
                    byte_counter_next = byte_counter + 8;  // Now at byte 16
                end
            end

            S_ORDER_REF: begin
                if (s_axis_tvalid) begin
                    state_next        = S_STOCK;
                    byte_counter_next = byte_counter + 8;  // Now at byte 24
                end
            end

            S_STOCK: begin
                if (s_axis_tvalid) begin
                    state_next        = S_PRICE;
                    byte_counter_next = byte_counter + 8;  // Now at byte 32
                end
            end

            S_PRICE: begin
                if (s_axis_tvalid) begin
                    // Message complete → return to IDLE for next message
                    state_next        = S_IDLE;
                    byte_counter_next = '0;
                end
            end

            S_SKIP: begin
                // Skip beats until we see TLAST (end of packet), then
                // return to IDLE to look for the next message.
                if (s_axis_tvalid && s_axis_tlast) begin
                    state_next        = S_IDLE;
                    byte_counter_next = '0;
                end
            end

            default: state_next = S_IDLE;
        endcase
    end

endmodule
