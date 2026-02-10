// =============================================================================
// AXI-Stream Package — Type Definitions for the Trading Engine
// =============================================================================
// Defines the standard AXI-Stream interface signals and ITCH-specific types
// used throughout the pipeline.

package axi_stream_pkg;

    // -------------------------------------------------------------------------
    // AXI-Stream Parameters
    // -------------------------------------------------------------------------
    parameter int AXIS_DATA_WIDTH = 64;  // 64-bit data bus (8 bytes per beat)
    parameter int AXIS_KEEP_WIDTH = AXIS_DATA_WIDTH / 8;

    // -------------------------------------------------------------------------
    // AXI-Stream Interface Signals (as a struct)
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic [AXIS_DATA_WIDTH-1:0] tdata;
        logic [AXIS_KEEP_WIDTH-1:0] tkeep;
        logic                       tvalid;
        logic                       tlast;
        // tready flows in the opposite direction (slave → master)
    } axis_master_t;

    // -------------------------------------------------------------------------
    // ITCH 5.0 Message Types
    // -------------------------------------------------------------------------
    typedef enum logic [7:0] {
        ITCH_SYSTEM_EVENT     = 8'h53,  // 'S'
        ITCH_STOCK_DIRECTORY  = 8'h52,  // 'R'
        ITCH_ADD_ORDER        = 8'h41,  // 'A'
        ITCH_ADD_ORDER_MPID   = 8'h46,  // 'F'
        ITCH_ORDER_EXECUTED   = 8'h45,  // 'E'
        ITCH_ORDER_CANCEL     = 8'h58,  // 'X'
        ITCH_ORDER_DELETE     = 8'h44,  // 'D'
        ITCH_ORDER_REPLACE    = 8'h55,  // 'U'
        ITCH_TRADE            = 8'h50   // 'P'
    } itch_msg_type_t;

    // -------------------------------------------------------------------------
    // Parsed ITCH Add Order — Output of the Parser
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic                valid;
        logic [63:0]         order_ref;     // Order reference number
        logic                side;          // 0 = Buy, 1 = Sell
        logic [31:0]         shares;        // Number of shares
        logic [63:0]         stock;         // Stock symbol (8 ASCII bytes)
        logic [31:0]         price;         // Price (4 decimal places implied)
        logic [47:0]         timestamp;     // Nanosecond timestamp
    } parsed_add_order_t;

    // -------------------------------------------------------------------------
    // MoE Feature Vector — Input to the Router
    // -------------------------------------------------------------------------
    parameter int NUM_FEATURES = 8;
    parameter int FEATURE_WIDTH = 16;  // ap_fixed<16,8> equivalent

    typedef logic signed [FEATURE_WIDTH-1:0] feature_t;

    typedef struct packed {
        logic                            valid;
        feature_t [NUM_FEATURES-1:0]     features;
    } moe_input_t;

    // -------------------------------------------------------------------------
    // MoE Output — Trade Signal
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic        valid;
        logic [1:0]  action;     // 00=Hold, 01=Buy, 10=Sell
        logic [15:0] confidence; // Fixed-point confidence score
        logic [31:0] price;      // Suggested limit price
        logic [31:0] quantity;   // Suggested quantity
    } trade_signal_t;

    // -------------------------------------------------------------------------
    // Order Book Entry
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic        valid;
        logic [31:0] price;
        logic [31:0] quantity;
        logic [63:0] order_ref;
    } ob_entry_t;

endpackage
