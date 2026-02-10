// =============================================================================
// ITCH 5.0 Line-Rate Parser ("The Shell")
// =============================================================================
// Processes 64-bit AXI-Stream beats from a 10GbE MAC and extracts "Add Order"
// messages (Type 0x41 / 'A') byte-by-byte without buffering entire packets.
//
// ITCH Add Order Message Layout (36 bytes):
//   Offset  Size  Field
//   0       1     Message Type ('A' = 0x41)
//   1       2     Stock Locate
//   3       2     Tracking Number
//   5       6     Timestamp (nanoseconds since midnight)
//   11      8     Order Reference Number
//   19      1     Buy/Sell Indicator ('B' or 'S')
//   20      4     Shares
//   24      8     Stock (right-padded ASCII)
//   32      4     Price (4 implied decimal places)
//
// Design: A simple FSM that counts bytes received. On each AXI-Stream beat,
// it extracts the relevant bytes into output registers. When a full Add Order
// message is consumed, it asserts `order_valid` for exactly one cycle.

module itch_parser
    import axi_stream_pkg::*;
(
    input  logic                        clk,
    input  logic                        rst_n,

    // AXI-Stream Slave (from 10GbE MAC)
    input  logic [AXIS_DATA_WIDTH-1:0]  s_axis_tdata,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [AXIS_KEEP_WIDTH-1:0]  s_axis_tkeep,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                        s_axis_tvalid,
    input  logic                        s_axis_tlast,
    output logic                        s_axis_tready,

    // Parsed output
    output parsed_add_order_t           order_out,

    // Statistics
    output logic [63:0]                 msg_count,
    output logic [63:0]                 add_order_count
);

    // -------------------------------------------------------------------------
    // FSM States
    // -------------------------------------------------------------------------
    typedef enum logic [2:0] {
        S_IDLE,         // Waiting for message type byte
        S_HEADER,       // Consuming header fields (locate, tracking, timestamp)
        S_ORDER_REF,    // Consuming order reference number
        S_SIDE_SHARES,  // Consuming side indicator and shares
        S_STOCK,        // Consuming stock symbol
        S_PRICE,        // Consuming price field
        S_SKIP          // Skipping non-Add-Order message
    } state_t;

    state_t state, state_next;

    // -------------------------------------------------------------------------
    // Internal Registers
    // -------------------------------------------------------------------------
    logic [5:0]  byte_counter;      // Counts bytes within current message
    logic [5:0]  byte_counter_next;

    // Accumulation registers for multi-beat fields
    logic [47:0] timestamp_acc;
    logic [63:0] order_ref_acc;
    logic [31:0] shares_acc;
    logic [63:0] stock_acc;
    logic [31:0] price_acc;
    logic        side_acc;

    // -------------------------------------------------------------------------
    // Always accept data (backpressure-free for line-rate operation)
    // -------------------------------------------------------------------------
    assign s_axis_tready = 1'b1;

    // -------------------------------------------------------------------------
    // Byte extraction helper — get byte N from the current 64-bit beat
    // ITCH is big-endian; AXI-Stream byte 0 is tdata[63:56]
    // -------------------------------------------------------------------------
    function automatic logic [7:0] get_byte(
        input logic [63:0] data,
        input int unsigned idx  // 0..7 within the beat
    );
        return data[(7 - idx) * 8 +: 8];
    endfunction

    // -------------------------------------------------------------------------
    // FSM: Sequential
    // -------------------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
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
            // Default: deassert valid after one cycle
            order_out.valid <= 1'b0;

            if (s_axis_tvalid && s_axis_tready) begin
                state        <= state_next;
                byte_counter <= byte_counter_next;

                case (state)
                    S_IDLE: begin
                        // Byte 0 of the beat is the message type
                        if (get_byte(s_axis_tdata, 0) == ITCH_ADD_ORDER) begin
                            msg_count <= msg_count + 1;
                            // Start accumulating — bytes 1-4 are locate + tracking
                            // Bytes 5-7 are first 3 bytes of timestamp
                            timestamp_acc[47:24] <= {get_byte(s_axis_tdata, 5),
                                                     get_byte(s_axis_tdata, 6),
                                                     get_byte(s_axis_tdata, 7)};
                        end else begin
                            msg_count <= msg_count + 1;
                        end
                    end

                    S_HEADER: begin
                        // Beat 1: bytes 8-15
                        // Timestamp bytes 3-5 are at positions 0-2 of this beat
                        timestamp_acc[23:0] <= {get_byte(s_axis_tdata, 0),
                                                get_byte(s_axis_tdata, 1),
                                                get_byte(s_axis_tdata, 2)};
                        // Order ref bytes 0-4 at positions 3-7
                        order_ref_acc[63:24] <= {get_byte(s_axis_tdata, 3),
                                                 get_byte(s_axis_tdata, 4),
                                                 get_byte(s_axis_tdata, 5),
                                                 get_byte(s_axis_tdata, 6),
                                                 get_byte(s_axis_tdata, 7)};
                    end

                    S_ORDER_REF: begin
                        // Beat 2: bytes 16-23
                        // Order ref bytes 5-7 at positions 0-2
                        order_ref_acc[23:0] <= {get_byte(s_axis_tdata, 0),
                                                get_byte(s_axis_tdata, 1),
                                                get_byte(s_axis_tdata, 2)};
                        // Side indicator at position 3
                        side_acc <= (get_byte(s_axis_tdata, 3) == 8'h53); // 'S' = Sell
                        // Shares at positions 4-7
                        shares_acc <= {get_byte(s_axis_tdata, 4),
                                       get_byte(s_axis_tdata, 5),
                                       get_byte(s_axis_tdata, 6),
                                       get_byte(s_axis_tdata, 7)};
                    end

                    S_STOCK: begin
                        // Beat 3: bytes 24-31 = stock symbol (8 bytes)
                        stock_acc <= {get_byte(s_axis_tdata, 0),
                                      get_byte(s_axis_tdata, 1),
                                      get_byte(s_axis_tdata, 2),
                                      get_byte(s_axis_tdata, 3),
                                      get_byte(s_axis_tdata, 4),
                                      get_byte(s_axis_tdata, 5),
                                      get_byte(s_axis_tdata, 6),
                                      get_byte(s_axis_tdata, 7)};
                    end

                    S_PRICE: begin
                        // Beat 4: bytes 32-35 = price (only first 4 bytes valid)
                        price_acc <= {get_byte(s_axis_tdata, 0),
                                      get_byte(s_axis_tdata, 1),
                                      get_byte(s_axis_tdata, 2),
                                      get_byte(s_axis_tdata, 3)};

                        // Emit the parsed order
                        order_out.valid     <= 1'b1;
                        order_out.order_ref <= order_ref_acc;
                        order_out.side      <= side_acc;
                        order_out.shares    <= shares_acc;
                        order_out.stock     <= stock_acc;
                        order_out.price     <= price_acc;
                        order_out.timestamp <= timestamp_acc;

                        add_order_count <= add_order_count + 1;
                    end

                    default: ;
                endcase
            end
        end
    end

    // -------------------------------------------------------------------------
    // FSM: Combinational Next-State Logic
    // -------------------------------------------------------------------------
    always_comb begin
        state_next        = state;
        byte_counter_next = byte_counter;

        case (state)
            S_IDLE: begin
                if (s_axis_tvalid) begin
                    if (get_byte(s_axis_tdata, 0) == ITCH_ADD_ORDER) begin
                        state_next        = S_HEADER;
                        byte_counter_next = 6'd8;
                    end else begin
                        state_next = S_SKIP;
                    end
                end
            end

            S_HEADER: begin
                if (s_axis_tvalid) begin
                    state_next        = S_ORDER_REF;
                    byte_counter_next = byte_counter + 8;
                end
            end

            S_ORDER_REF: begin
                if (s_axis_tvalid) begin
                    state_next        = S_STOCK;
                    byte_counter_next = byte_counter + 8;
                end
            end

            S_STOCK: begin
                if (s_axis_tvalid) begin
                    state_next        = S_PRICE;
                    byte_counter_next = byte_counter + 8;
                end
            end

            S_PRICE: begin
                if (s_axis_tvalid) begin
                    state_next        = S_IDLE;
                    byte_counter_next = '0;
                end
            end

            S_SKIP: begin
                // Skip until TLAST
                if (s_axis_tvalid && s_axis_tlast) begin
                    state_next        = S_IDLE;
                    byte_counter_next = '0;
                end
            end

            default: state_next = S_IDLE;
        endcase
    end

endmodule
