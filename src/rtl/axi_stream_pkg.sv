// =============================================================================
// AXI-Stream Package — Type Definitions for the Trading Engine
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Defines all the data types and constants used throughout the hardware
//   design. Think of it as a "dictionary" — every other file imports this
//   to know what the data looks like.
//
// WHY A PACKAGE?
//   In SystemVerilog, a "package" is like a C++ header file — it lets
//   multiple modules share the same type definitions without duplicating
//   code. Any module that says `import axi_stream_pkg::*` gets access
//   to everything defined here.
//
// KEY CONCEPTS:
//   - AXI-Stream: A standard protocol for moving data between FPGA blocks
//   - ITCH 5.0:   NASDAQ's binary market data format
//   - Packed structs: Bit-level layout matters in hardware (unlike software)
//   - Fixed-point:  Hardware-friendly alternative to floating-point
// =============================================================================

package axi_stream_pkg;

    // -------------------------------------------------------------------------
    // AXI-Stream Parameters
    // -------------------------------------------------------------------------
    // The data bus width matches a 10GbE MAC: 64 bits = 8 bytes per clock.
    // At 250 MHz, this gives 250M × 8 = 2 GB/s = 16 Gbps — enough for 10GbE
    // with protocol overhead.
    parameter int AXIS_DATA_WIDTH = 64;  // 64-bit data bus (8 bytes per beat)
    parameter int AXIS_KEEP_WIDTH = AXIS_DATA_WIDTH / 8; // 8 bits — one per byte

    // -------------------------------------------------------------------------
    // AXI-Stream Interface Signals (as a struct)
    // -------------------------------------------------------------------------
    // AXI-Stream is like a conveyor belt for data:
    //   tdata  = the package on the belt (64 bits of data)
    //   tkeep  = which bytes in tdata are valid (e.g., last beat may have < 8)
    //   tvalid = "I'm putting a package on the belt right now"
    //   tlast  = "this is the last package in this group"
    //   tready = "the receiver is ready" (flows in opposite direction)
    //
    // A data transfer happens ONLY when tvalid AND tready are both 1
    // on the same clock edge. This is called a "handshake."
    typedef struct packed {
        logic [AXIS_DATA_WIDTH-1:0] tdata;   // The actual data bytes
        logic [AXIS_KEEP_WIDTH-1:0] tkeep;   // Byte enable: 1=valid, 0=padding
        logic                       tvalid;  // Master says "data is ready"
        logic                       tlast;   // Master says "end of packet"
        // tready flows in the opposite direction (slave → master)
        // so it's NOT in this struct — it's a separate signal on the module port.
    } axis_master_t;

    // -------------------------------------------------------------------------
    // ITCH 5.0 Message Types
    // -------------------------------------------------------------------------
    // ITCH uses a single byte to identify message types. These ASCII codes
    // are defined by NASDAQ's specification. We only parse 'A' (Add Order)
    // but define others for completeness and future expansion.
    //
    // In hardware, this becomes a simple 8-bit comparison: if the first byte
    // of a packet equals 0x41, it's an Add Order. No string matching needed.
    typedef enum logic [7:0] {
        ITCH_SYSTEM_EVENT     = 8'h53,  // 'S' — Market open/close events
        ITCH_STOCK_DIRECTORY  = 8'h52,  // 'R' — Stock listing info
        ITCH_ADD_ORDER        = 8'h41,  // 'A' — New order added to book ← WE PARSE THIS
        ITCH_ADD_ORDER_MPID   = 8'h46,  // 'F' — Add order with market participant ID
        ITCH_ORDER_EXECUTED   = 8'h45,  // 'E' — Existing order was filled
        ITCH_ORDER_CANCEL     = 8'h58,  // 'X' — Partial order cancellation
        ITCH_ORDER_DELETE     = 8'h44,  // 'D' — Full order removal
        ITCH_ORDER_REPLACE    = 8'h55,  // 'U' — Order price/quantity change
        ITCH_TRADE            = 8'h50   // 'P' — Non-displayable trade
    } itch_msg_type_t;

    // -------------------------------------------------------------------------
    // Parsed ITCH Add Order — Output of the Parser
    // -------------------------------------------------------------------------
    // After the parser processes an ITCH Add Order message, it outputs this
    // struct with all fields extracted and aligned. The `valid` flag is high
    // for exactly one clock cycle when a new order is ready.
    //
    // "packed" means the bits are laid out contiguously with no padding:
    //   Total size = 1 + 64 + 1 + 32 + 64 + 32 + 48 = 242 bits
    //
    // Field sizes match the ITCH 5.0 specification exactly.
    typedef struct packed {
        logic                valid;        // 1 = new order ready this cycle
        logic [63:0]         order_ref;    // Unique ID assigned by NASDAQ
        logic                side;         // 0 = Buy ('B'), 1 = Sell ('S')
        logic [31:0]         shares;       // Number of shares (e.g., 100)
        logic [63:0]         stock;        // 8-byte ASCII ticker (e.g., "AAPL    ")
        logic [31:0]         price;        // Price × 10000 (e.g., $150.25 = 1502500)
        logic [47:0]         timestamp;    // Nanoseconds since midnight (6 bytes)
    } parsed_add_order_t;

    // -------------------------------------------------------------------------
    // MoE Feature Vector — Input to the Router
    // -------------------------------------------------------------------------
    // The MoE model takes 8 fixed-point features as input. These represent
    // market microstructure signals extracted from the order flow:
    //   Feature 0: Normalized price (relative to midpoint)
    //   Feature 1: Side indicator (+1 buy, -1 sell)
    //   Feature 2: Log quantity
    //   Feature 3: Spread
    //   Feature 4: Price distance from best
    //   Features 5-7: Reserved for rolling statistics
    //
    // FEATURE_WIDTH = 16 bits matches ap_fixed<16,8> in HLS:
    //   8 integer bits + 8 fractional bits
    //   Range: -128.0 to +127.996
    //   Resolution: 1/256 ≈ 0.004
    parameter int NUM_FEATURES = 8;
    parameter int FEATURE_WIDTH = 16;  // ap_fixed<16,8> equivalent

    // signed = two's complement, allowing negative values
    typedef logic signed [FEATURE_WIDTH-1:0] feature_t;

    typedef struct packed {
        logic                            valid;     // 1 = features ready
        feature_t [NUM_FEATURES-1:0]     features;  // 8 × 16-bit = 128 bits
    } moe_input_t;

    // -------------------------------------------------------------------------
    // MoE Output — Trade Signal
    // -------------------------------------------------------------------------
    // The final output of the MoE engine: a trading decision.
    //   action: 00 = Hold (do nothing), 01 = Buy, 10 = Sell
    //   confidence: How sure the model is (higher = more confident)
    //   price: Suggested limit price for the order
    //   quantity: Suggested number of shares
    typedef struct packed {
        logic        valid;       // 1 = trade signal ready
        logic [1:0]  action;      // 2-bit action code (Hold/Buy/Sell)
        logic [15:0] confidence;  // Fixed-point confidence score
        logic [31:0] price;       // Suggested limit price
        logic [31:0] quantity;    // Suggested quantity
    } trade_signal_t;

    // -------------------------------------------------------------------------
    // Order Book Entry
    // -------------------------------------------------------------------------
    // Represents one entry in the Limit Order Book. Used by the matching
    // engine to track outstanding orders at each price level.
    typedef struct packed {
        logic        valid;       // 1 = this entry is occupied
        logic [31:0] price;       // Price level
        logic [31:0] quantity;    // Aggregate quantity at this level
        logic [63:0] order_ref;   // Reference number of the oldest order
    } ob_entry_t;

endpackage
