// =============================================================================
// Top-Level Module — FPGA Trading Engine
// =============================================================================
// Integrates the ITCH parser, MoE engine, and matching engine.
// For initial development, the MoE and matching engine are stubbed out
// and will be replaced with HLS-generated IP blocks.

module top
    import axi_stream_pkg::*;
(
    input  logic                        clk,
    input  logic                        rst_n,

    // AXI-Stream Slave — from 10GbE MAC
    input  logic [AXIS_DATA_WIDTH-1:0]  s_axis_tdata,
    input  logic [AXIS_KEEP_WIDTH-1:0]  s_axis_tkeep,
    input  logic                        s_axis_tvalid,
    input  logic                        s_axis_tlast,
    output logic                        s_axis_tready,

    // Trade signal output
    output trade_signal_t               trade_out,

    // Debug / statistics
    output logic [63:0]                 total_messages,
    output logic [63:0]                 total_add_orders
);

    // -------------------------------------------------------------------------
    // Internal Signals
    // -------------------------------------------------------------------------
    parsed_add_order_t parsed_order;

    // -------------------------------------------------------------------------
    // Stage 1: ITCH Parser ("The Shell")
    // -------------------------------------------------------------------------
    itch_parser u_parser (
        .clk              (clk),
        .rst_n            (rst_n),
        .s_axis_tdata     (s_axis_tdata),
        .s_axis_tkeep     (s_axis_tkeep),
        .s_axis_tvalid    (s_axis_tvalid),
        .s_axis_tlast     (s_axis_tlast),
        .s_axis_tready    (s_axis_tready),
        .order_out        (parsed_order),
        .msg_count        (total_messages),
        .add_order_count  (total_add_orders)
    );

    // -------------------------------------------------------------------------
    // Stage 2: MoE Engine ("The Brain") — STUB
    // -------------------------------------------------------------------------
    // TODO: Replace with HLS-generated MoE IP (moe_router + expert kernels)
    // For now, pass-through: every Add Order generates a buy signal
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            trade_out <= '0;
        end else if (parsed_order.valid) begin
            trade_out.valid      <= 1'b1;
            trade_out.action     <= 2'b01;  // Stub: always "Buy"
            trade_out.confidence <= 16'h7FFF;
            trade_out.price      <= parsed_order.price;
            trade_out.quantity   <= parsed_order.shares;
        end else begin
            trade_out.valid <= 1'b0;
        end
    end

endmodule
