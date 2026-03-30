// =========================================================================
// top.sv — Top-Level Integration: ITCH Parser + Order Book
//
// Wires together:
//   1. itch_parser: raw AXI-Stream bytes → parsed message fields
//   2. order_book:  parsed messages → best bid/ask + book state
//   3. latency_counter: measures cycles from first input byte to
//      valid book state output (wire-to-response metric)
//
// This top-level exposes the same AXI-Stream input interface that would
// connect to a 10 GbE MAC in a full FPGA implementation. The MoE HLS
// IP would connect downstream of order_book (placeholder shown below).
//
// Latency measurement:
//   t_start: cycle when s_axis_tvalid first goes high for a new message
//   t_end:   cycle when book_valid asserts (both bid and ask known)
//   wire_to_response_ns = (t_end - t_start) * 4  (at 250 MHz)
// =========================================================================

// Suppress MODDUP: Verilator finds this module twice when -y library path is
// used alongside the explicit file path. Behavior is correct; both paths
// refer to the same file.
/* verilator lint_off MODDUP */
module top (
    input  logic        clk,
    input  logic        rst_n,

    // Raw AXI4-Stream input from MAC / testbench
    input  logic        s_axis_tvalid,
    output logic        s_axis_tready,
    input  logic [63:0] s_axis_tdata,
    input  logic [7:0]  s_axis_tkeep,
    input  logic        s_axis_tlast,

    // Book state outputs
    output logic [31:0] best_bid,
    output logic [31:0] best_ask,
    output logic [31:0] spread,
    output logic [31:0] mid_price,
    output logic        book_valid,

    // Trade signal placeholder (would be driven by MoE HLS IP)
    // 2'b00 = HOLD, 2'b01 = BUY, 2'b10 = SELL
    output logic [1:0]  trade_signal,
    output logic        trade_valid,

    // Latency counter — cycles from s_axis_tvalid to book_valid
    output logic [15:0] latency_counter
);

// -------------------------------------------------------------------------
// Internal wires connecting itch_parser → order_book
// -------------------------------------------------------------------------
logic        parser_msg_valid;
logic        parser_msg_ready;
logic [7:0]  parser_msg_type;
logic [63:0] parser_order_ref;
logic        parser_side;
logic [31:0] parser_shares;
logic [31:0] parser_price;
/* verilator lint_off UNUSED */
logic [15:0] parser_stock_locate;  // wired from parser; not forwarded to order book
logic [15:0] ob_latency_cycles;    // order_book internal latency output (unused; top counts separately)
/* verilator lint_on UNUSED */

// -------------------------------------------------------------------------
// ITCH Parser instance
// -------------------------------------------------------------------------
itch_parser u_itch_parser (
    .clk             (clk),
    .rst_n           (rst_n),

    .s_axis_tvalid   (s_axis_tvalid),
    .s_axis_tready   (s_axis_tready),
    .s_axis_tdata    (s_axis_tdata),
    .s_axis_tkeep    (s_axis_tkeep),
    .s_axis_tlast    (s_axis_tlast),

    .m_axis_tvalid   (parser_msg_valid),
    .m_axis_tready   (parser_msg_ready),
    .m_axis_msg_type (parser_msg_type),
    .m_axis_order_ref(parser_order_ref),
    .m_axis_side     (parser_side),
    .m_axis_shares   (parser_shares),
    .m_axis_price    (parser_price),
    .m_axis_stock_locate(parser_stock_locate)
);

// -------------------------------------------------------------------------
// Order Book instance
// -------------------------------------------------------------------------
order_book u_order_book (
    .clk           (clk),
    .rst_n         (rst_n),

    .msg_valid     (parser_msg_valid),
    .msg_ready     (parser_msg_ready),
    .msg_type      (parser_msg_type),
    .order_ref     (parser_order_ref),
    .side          (parser_side),
    .shares        (parser_shares),
    .price         (parser_price),

    .best_bid      (best_bid),
    .best_ask      (best_ask),
    .spread        (spread),
    .mid_price     (mid_price),
    .book_valid    (book_valid),
    .latency_cycles(ob_latency_cycles)
);

// -------------------------------------------------------------------------
// Latency counter: tracks cycles from first s_axis_tvalid to book_valid
//
// This counts the number of clock cycles between the first beat of a new
// ITCH message arriving on the AXI-Stream bus and the book_valid signal
// asserting (indicating both best_bid and best_ask have been updated).
//
// In the Verilator sim, the C++ harness samples this register after each
// message to record per-message latency.
// -------------------------------------------------------------------------
logic [15:0] r_latency_reg;
logic        r_counting;
logic        r_prev_valid;

always_ff @(posedge clk) begin
    if (!rst_n) begin
        r_latency_reg <= 16'h0;
        r_counting    <= 1'b0;
        r_prev_valid  <= 1'b0;
    end else begin
        r_prev_valid <= s_axis_tvalid;

        // Start counting when first beat of new message arrives
        if (s_axis_tvalid && !r_prev_valid) begin
            r_latency_reg <= 16'h0;
            r_counting    <= 1'b1;
        end

        // Increment counter while waiting
        if (r_counting)
            r_latency_reg <= r_latency_reg + 1'b1;

        // Stop when book becomes valid (and stop overflow)
        if (book_valid) begin
            r_counting <= 1'b0;
        end
    end
end

assign latency_counter = r_latency_reg;

// -------------------------------------------------------------------------
// MoE trade signal placeholder
//
// In the full design, best_bid, best_ask, spread, and mid_price feed into
// the HLS MoE IP via AXI-Stream. The IP computes the trade signal.
// Here we implement a trivial rule-based placeholder to keep the pipeline
// connected for latency measurement:
//   BUY  if spread < 100 ($0.01) and book_valid
//   SELL never (placeholder)
//   HOLD otherwise
//
// Latency of this "MoE placeholder" = 1 cycle (combinatorial).
// The real HLS MoE IP would add ~8 cycles per the synthesis report.
// -------------------------------------------------------------------------
always_ff @(posedge clk) begin
    if (!rst_n) begin
        trade_signal <= 2'b00;
        trade_valid  <= 1'b0;
    end else if (book_valid) begin
        trade_valid <= 1'b1;
        if (spread < 32'd100)
            trade_signal <= 2'b01;  // BUY
        else
            trade_signal <= 2'b00;  // HOLD
    end else begin
        trade_valid <= 1'b0;
    end
end

endmodule
/* verilator lint_on MODDUP */
