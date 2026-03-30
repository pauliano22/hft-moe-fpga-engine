// =========================================================================
// itch_parser.sv — AXI-Stream NASDAQ ITCH 5.0 Binary Parser
//
// Ingests raw ITCH 5.0 binary stream bytes on a 64-bit AXI4-Stream bus
// (8 bytes per clock at 250 MHz = 2 GB/s — matching 10 GbE wire rate).
// Parses the 2-byte length prefix and message body, then emits a
// structured ParsedMsg on an AXI4-Stream output.
//
// Supported message types: A (Add), D (Delete), E (Execute), X (Cancel)
// All other types are consumed and discarded (output valid not asserted).
//
// AXI-Stream byte ordering:
//   TDATA[7:0]   = byte 0 (first byte arriving on wire)
//   TDATA[15:8]  = byte 1
//   ...
//   TDATA[63:56] = byte 7
// So byte N within a 64-bit beat: TDATA[(N%8)*8 +: 8]
//
// State machine:
//   IDLE       → waiting for first byte of a new message
//   PARSE_B0   → beat 0 (bytes 0–7): length prefix, msg_type, header start
//   PARSE_B1   → beat 1 (bytes 8–15): timestamp remainder, order_ref start
//   PARSE_B2   → beat 2 (bytes 16–23): order_ref remainder, side, shares start
//   PARSE_B3   → beat 3 (bytes 24–31): shares end, stock start
//   PARSE_B4   → beat 4 (bytes 32–37): stock end, price — last beat
//   EMIT       → assert m_axis_tvalid for one cycle
//
// Latency: 5 beats × 4 ns/beat = 20 ns from first byte to parsed output.
// This is the "first stage" of the wire-to-response pipeline.
// =========================================================================

module itch_parser (
    input  logic        clk,
    input  logic        rst_n,      // active-low synchronous reset

    // AXI4-Stream Slave (raw ITCH bytes from MAC)
    input  logic        s_axis_tvalid,
    output logic        s_axis_tready,
    input  logic [63:0] s_axis_tdata,
    /* verilator lint_off UNUSED */
    input  logic [7:0]  s_axis_tkeep,  // required by AXI-Stream spec; not used in this parser
    input  logic        s_axis_tlast,  // single-message framing handled by length prefix
    /* verilator lint_on UNUSED */

    // AXI4-Stream Master (parsed message to order book)
    output logic        m_axis_tvalid,
    input  logic        m_axis_tready,
    output logic [7:0]  m_axis_msg_type,    // 'A', 'D', 'E', 'X'
    output logic [63:0] m_axis_order_ref,   // order reference number
    output logic        m_axis_side,        // 0=bid, 1=ask (Add only)
    output logic [31:0] m_axis_shares,      // quantity
    output logic [31:0] m_axis_price,       // price × 10000 (Add only)
    output logic [15:0] m_axis_stock_locate // stock identifier
);

// -------------------------------------------------------------------------
// State encoding — one-hot for Xilinx LUT optimization
// -------------------------------------------------------------------------
typedef enum logic [2:0] {
    IDLE     = 3'b000,
    PARSE_B0 = 3'b001,   // beat 0: bytes [0:7]  — length + type + header
    PARSE_B1 = 3'b010,   // beat 1: bytes [8:15] — timestamp[1:5] + ref[0:2]
    PARSE_B2 = 3'b011,   // beat 2: bytes [16:23]— ref[3:7] + side + shares[0:1]
    PARSE_B3 = 3'b100,   // beat 3: bytes [24:31]— shares[2:3] + stock[0:5]
    PARSE_B4 = 3'b101,   // beat 4: bytes [32:37]— stock[6:7] + price[0:3]
    EMIT     = 3'b110    // output valid for one cycle
} state_t;

state_t state, next_state;

// -------------------------------------------------------------------------
// Registered field storage — hold extracted bytes as they arrive
// Fields are big-endian in ITCH but accumulated MSB-first here
// -------------------------------------------------------------------------
logic [7:0]  r_msg_type;
logic [15:0] r_msg_len;
logic [15:0] r_stock_locate;
logic [63:0] r_order_ref;
logic        r_side;
logic [31:0] r_shares;
/* verilator lint_off UNUSED */
logic [63:0] r_stock;       // 8-byte ticker (kept as raw bytes; not forwarded downstream)
/* verilator lint_on UNUSED */
logic [31:0] r_price;
logic        r_valid;       // output valid flag

// -------------------------------------------------------------------------
// Helper: extract byte N from the current 64-bit beat (little-endian bus)
// -------------------------------------------------------------------------
// Byte N within a beat is at TDATA[(N%8)*8 +: 8]
// For bytes at absolute stream offset O: beat = O/8, offset_in_beat = O%8
`define BYTE(beat_data, offset_in_beat) (beat_data[((offset_in_beat)*8) +: 8])

// -------------------------------------------------------------------------
// s_axis_tready — we accept data whenever we are not in EMIT state
// (EMIT lasts one cycle; the upstream can send immediately after)
// -------------------------------------------------------------------------
assign s_axis_tready = (state != EMIT) && rst_n;

// -------------------------------------------------------------------------
// State register
// -------------------------------------------------------------------------
always_ff @(posedge clk) begin
    if (!rst_n)
        state <= IDLE;
    else
        state <= next_state;
end

// -------------------------------------------------------------------------
// Field extraction and state transitions
//
// All fields are extracted on the cycle when the beat arrives (s_axis_tvalid
// & s_axis_tready). The state advances on the same clock edge.
//
// Message layout in the raw stream (including 2-byte length prefix):
//
//   Offset  Field           Size
//   ------  -----           ----
//   0-1     length prefix   2 B  (big-endian, value = message body size)
//   2       msg_type        1 B  ('A', 'D', 'E', 'X', ...)
//   3-4     stock_locate    2 B  (big-endian)
//   5-6     tracking_num    2 B  (ignored)
//   7-12    timestamp       6 B  (ignored for latency — we only care about fields)
//   13-20   order_ref_num   8 B  (big-endian) [for D/E/X: 11-18 in body = 13-20 w/ prefix]
//   21      side            1 B  ('B'=0x42, 'S'=0x53) [Add only]
//   22-25   shares          4 B  [Add: body 20-23; Delete: N/A]
//   26-33   stock           8 B  [Add only]
//   34-37   price           4 B  [Add only]
// -------------------------------------------------------------------------
always_ff @(posedge clk) begin
    if (!rst_n) begin
        r_msg_type    <= '0;
        r_msg_len     <= '0;
        r_stock_locate<= '0;
        r_order_ref   <= '0;
        r_side        <= '0;
        r_shares      <= '0;
        r_stock       <= '0;
        r_price       <= '0;
        r_valid       <= '0;
    end else begin
        r_valid <= '0;  // default: clear valid; EMIT state will set it

        case (state)
            // ----------------------------------------------------------
            // IDLE: wait for first beat of new message.
            //
            // Beat 0 arrives while in IDLE — the AXI-Stream handshake
            // (tvalid && tready) fires here because tready = (state != EMIT).
            // We MUST capture beat 0's fields in IDLE; by the next clock the
            // machine is in PARSE_B1 and beat 0 is gone from the bus.
            //
            // BEAT 0 (stream offsets 0–7):
            //   [0:1] = length prefix (big-endian)
            //   [2]   = msg_type
            //   [3:4] = stock_locate (big-endian)
            //   [5:6] = tracking_num (ignored)
            //   [7]   = timestamp[0] (ignored)
            // ----------------------------------------------------------
            IDLE: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    r_msg_len     <= {`BYTE(s_axis_tdata, 0), `BYTE(s_axis_tdata, 1)};
                    r_msg_type    <= `BYTE(s_axis_tdata, 2);
                    r_stock_locate<= {`BYTE(s_axis_tdata, 3), `BYTE(s_axis_tdata, 4)};
                    // bytes [5:7]: tracking_num + timestamp[0] — discard
                end
            end

            // ----------------------------------------------------------
            // PARSE_B0: dead state — beat 0 is now captured in IDLE.
            // IDLE transitions directly to PARSE_B1, so this state is
            // unreachable. Kept for enum encoding completeness.
            // ----------------------------------------------------------
            PARSE_B0: begin
                // unreachable
            end

            // ----------------------------------------------------------
            // BEAT 1 (stream offsets 8–15):
            //   [8:12]  = timestamp[1:5] (ignored)
            //   [13:15] = order_ref_num[56:32] (first 3 bytes)
            // ----------------------------------------------------------
            PARSE_B1: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    // bytes [0:4] of this beat = timestamp[1:5] — discard
                    // bytes [5:7] = order_ref_num bits [63:40]
                    r_order_ref[63:40] <= {`BYTE(s_axis_tdata, 5),
                                           `BYTE(s_axis_tdata, 6),
                                           `BYTE(s_axis_tdata, 7)};
                end
            end

            // ----------------------------------------------------------
            // BEAT 2 (stream offsets 16–23):
            //   [16:20] = order_ref_num[39:0] (remaining 5 bytes)
            //   [21]    = side ('B'=0x42, 'S'=0x53) — Add only
            //   [22:23] = shares[31:16] — Add only
            // ----------------------------------------------------------
            PARSE_B2: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    r_order_ref[39:0] <= {`BYTE(s_axis_tdata, 0),
                                          `BYTE(s_axis_tdata, 1),
                                          `BYTE(s_axis_tdata, 2),
                                          `BYTE(s_axis_tdata, 3),
                                          `BYTE(s_axis_tdata, 4)};
                    r_side   <= (`BYTE(s_axis_tdata, 5) == 8'h42) ? 1'b0 : 1'b1; // 'B'=bid
                    r_shares[31:16] <= {`BYTE(s_axis_tdata, 6), `BYTE(s_axis_tdata, 7)};
                end
            end

            // ----------------------------------------------------------
            // BEAT 3 (stream offsets 24–31):
            //   [24:25] = shares[15:0] — Add only
            //   [26:31] = stock[0:5] — Add only
            // ----------------------------------------------------------
            PARSE_B3: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    r_shares[15:0] <= {`BYTE(s_axis_tdata, 0), `BYTE(s_axis_tdata, 1)};
                    r_stock[63:16] <= {`BYTE(s_axis_tdata, 2), `BYTE(s_axis_tdata, 3),
                                       `BYTE(s_axis_tdata, 4), `BYTE(s_axis_tdata, 5),
                                       `BYTE(s_axis_tdata, 6), `BYTE(s_axis_tdata, 7)};
                end
            end

            // ----------------------------------------------------------
            // BEAT 4 (stream offsets 32–37, 6 bytes valid):
            //   [32:33] = stock[6:7] — Add only
            //   [34:37] = price[0:3] — Add only (big-endian)
            // ----------------------------------------------------------
            PARSE_B4: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    r_stock[15:0] <= {`BYTE(s_axis_tdata, 0), `BYTE(s_axis_tdata, 1)};
                    r_price <= {`BYTE(s_axis_tdata, 2), `BYTE(s_axis_tdata, 3),
                                `BYTE(s_axis_tdata, 4), `BYTE(s_axis_tdata, 5)};
                    r_valid <= 1'b1;
                end
            end

            // ----------------------------------------------------------
            // EMIT: hold valid for one cycle to allow downstream to latch
            // ----------------------------------------------------------
            EMIT: begin
                r_valid <= 1'b1;  // keep valid asserted until m_axis_tready
                if (m_axis_tready) r_valid <= 1'b0;
            end

            default: begin
                // Unreachable with 7-state one-hot enum; satisfies case completeness
            end

        endcase
    end
end

// -------------------------------------------------------------------------
// Next-state logic (combinatorial)
// Filters out unsupported message types early (IDLE → PARSE_B0 only on
// recognized types that need more than 1 beat to parse).
// -------------------------------------------------------------------------
always_comb begin
    next_state = state;

    case (state)
        IDLE: begin
            // Beat 0 is captured here (see always_ff above).
            // Filter on msg_type from live bus data to skip unsupported types.
            // msg_type is byte 2 of beat 0 = TDATA[23:16].
            // Note: do NOT use a local variable (e.g. logic mtype = tdata[...])
            // inside always_comb — Verilator treats initializers as static and
            // won't re-evaluate them on signal changes. Use inline comparisons.
            if (s_axis_tvalid) begin
                if (s_axis_tdata[23:16] == 8'h44 ||  // 'D' = Delete: 19 bytes
                    s_axis_tdata[23:16] == 8'h58 ||  // 'X' = Cancel: 23 bytes
                    s_axis_tdata[23:16] == 8'h45 ||  // 'E' = Execute:31 bytes
                    s_axis_tdata[23:16] == 8'h41 ||  // 'A' = Add:    36 bytes
                    s_axis_tdata[23:16] == 8'h46)    // 'F' = Add+MPID:40 bytes
                    next_state = PARSE_B1;  // beat 0 captured in IDLE → go to beat 1
                else
                    next_state = IDLE;  // unsupported type: skip (beat 0 consumed)
            end
        end

        PARSE_B0: begin
            // Dead state — unreachable. IDLE transitions directly to PARSE_B1.
            next_state = IDLE;
        end

        PARSE_B1: begin
            if (s_axis_tvalid && s_axis_tready) begin
                // 'D' (Delete) is fully parsed after beat 2 (19 bytes = ceil(19/8)=3 beats)
                if (r_msg_type == 8'h44 && r_msg_len <= 19)
                    next_state = PARSE_B2;
                else
                    next_state = PARSE_B2; // all types continue to beat 2
            end
        end

        PARSE_B2: begin
            if (s_axis_tvalid && s_axis_tready) begin
                // Delete (D): 19 bytes total (17 body + 2 len) fits in 3 beats
                // Cancel (X): 23 bytes = 3 beats (bytes 0-23)
                // Both are "done" after beat 2 (we have all needed fields)
                if (r_msg_type == 8'h44 || r_msg_type == 8'h58 || r_msg_type == 8'h45)
                    next_state = EMIT;
                else
                    next_state = PARSE_B3;
            end
        end

        PARSE_B3: begin
            if (s_axis_tvalid && s_axis_tready)
                next_state = PARSE_B4;
        end

        PARSE_B4: begin
            if (s_axis_tvalid && s_axis_tready)
                next_state = EMIT;
        end

        EMIT: begin
            if (m_axis_tready || !r_valid)
                next_state = IDLE;
        end

        default: next_state = IDLE;
    endcase
end

// -------------------------------------------------------------------------
// Output assignments — combinatorial from registered state
// -------------------------------------------------------------------------
assign m_axis_tvalid      = r_valid;
assign m_axis_msg_type    = r_msg_type;
assign m_axis_order_ref   = r_order_ref;
assign m_axis_side        = r_side;
assign m_axis_shares      = r_shares;
assign m_axis_price       = r_price;
assign m_axis_stock_locate = r_stock_locate;

endmodule
