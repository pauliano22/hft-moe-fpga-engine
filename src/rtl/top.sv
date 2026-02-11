// =============================================================================
// Top-Level Module — FPGA Trading Engine
// =============================================================================
//
// WHAT THIS MODULE DOES:
//   Wires together the full pipeline: ITCH parser → MoE engine → output.
//   This is the "roof" of the design — it instantiates all sub-modules and
//   connects their interfaces.
//
// CURRENT STATUS:
//   - Stage 1 (ITCH Parser): FULLY IMPLEMENTED in SystemVerilog
//   - Stage 2 (MoE Engine):  STUBBED — generates a dummy "Buy" signal
//                             Will be replaced with HLS-generated IP blocks
//   - Stage 3 (Matching):    Not yet connected (part of MoE stub)
//
// HOW TO READ THIS FILE:
//   1. Look at the port list — these are the chip's external connections
//   2. Look at the internal signal declarations
//   3. Follow the instantiations from Stage 1 → Stage 2 → output
//
// IN A REAL FPGA DESIGN:
//   This top-level would also include:
//   - Clock management (PLL/MMCM for clock generation)
//   - AXI interconnect for register access
//   - DMA engines for host communication
//   - Interrupt controllers
//   For this project, we keep it simple to focus on the data path.

module top
    import axi_stream_pkg::*;  // Import shared type definitions
(
    // -------------------------------------------------------------------------
    // Clock and Reset
    // -------------------------------------------------------------------------
    input  logic                        clk,     // System clock
    input  logic                        rst_n,   // Active-low synchronous reset

    // -------------------------------------------------------------------------
    // AXI-Stream Slave — from 10GbE MAC
    // -------------------------------------------------------------------------
    // Raw Ethernet frames arrive here, 64 bits at a time.
    // The ITCH parser inside this module processes these bytes.
    input  logic [AXIS_DATA_WIDTH-1:0]  s_axis_tdata,
    input  logic [AXIS_KEEP_WIDTH-1:0]  s_axis_tkeep,
    input  logic                        s_axis_tvalid,
    input  logic                        s_axis_tlast,
    output logic                        s_axis_tready,

    // -------------------------------------------------------------------------
    // Trade Signal Output
    // -------------------------------------------------------------------------
    // The final result: Buy, Sell, or Hold, with confidence and parameters.
    output trade_signal_t               trade_out,

    // -------------------------------------------------------------------------
    // Debug / Statistics
    // -------------------------------------------------------------------------
    // Exposed for monitoring via AXI-Lite register interface (future).
    output logic [63:0]                 total_messages,
    output logic [63:0]                 total_add_orders
);

    // -------------------------------------------------------------------------
    // Internal Signals
    // -------------------------------------------------------------------------
    // This wire carries the parsed order from Stage 1 to Stage 2.
    // It's valid for one cycle when the parser finishes an Add Order.
    parsed_add_order_t parsed_order;

    // -------------------------------------------------------------------------
    // Stage 1: ITCH Parser ("The Shell")
    // -------------------------------------------------------------------------
    // Instantiate the parser module and connect its ports.
    // The `.port_name(signal_name)` syntax connects module ports to our signals.
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
    //
    // For now, this is a passthrough stub: every Add Order generates a "Buy"
    // signal with maximum confidence. In the full implementation, this would:
    //   1. Extract features from the parsed order + order book state
    //   2. Run the MoE router to select top-2 experts
    //   3. Run the selected expert MLPs
    //   4. Combine expert outputs into a trade decision
    //   5. Feed the decision to the matching engine
    //
    // The stub demonstrates the interface contract: when parsed_order.valid
    // goes high, we produce a trade_out.valid one cycle later.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            trade_out <= '0;  // Zero all fields on reset
        end else if (parsed_order.valid) begin
            // Stub: map every parsed order to a Buy signal
            trade_out.valid      <= 1'b1;
            trade_out.action     <= 2'b01;       // Always "Buy" (stub)
            trade_out.confidence <= 16'h7FFF;    // Max confidence (stub)
            trade_out.price      <= parsed_order.price;
            trade_out.quantity   <= parsed_order.shares;
        end else begin
            trade_out.valid <= 1'b0;  // No signal when no order arrives
        end
    end

endmodule
