// =========================================================================
// order_book_tb.sv — Testbench for order_book.sv
// =========================================================================

`timescale 1ns/1ps

module order_book_tb;

logic        clk = 0;
logic        rst_n;
logic        msg_valid;
logic        msg_ready;
logic [7:0]  msg_type;
logic [63:0] order_ref;
logic        side;
logic [31:0] shares;
logic [31:0] price;
logic [31:0] best_bid, best_ask, spread, mid_price;
logic        book_valid;
logic [15:0] latency_cycles;

order_book dut (.*);

always #2 clk = ~clk;

int tests_run = 0, tests_passed = 0;

task check32(string name, logic [31:0] got, logic [31:0] exp);
    if (got !== exp)
        $display("  FAIL %s: got=%0d exp=%0d", name, got, exp);
    else
        $display("  OK   %s = %0d", name, got);
endtask

task send_msg(
    input logic [7:0]  mtype,
    input logic        mside,
    input logic [63:0] mref,
    input logic [31:0] mshares,
    input logic [31:0] mprice
);
    @(posedge clk);
    msg_valid  <= 1'b1;
    msg_type   <= mtype;
    side       <= mside;
    order_ref  <= mref;
    shares     <= mshares;
    price      <= mprice;
    @(posedge clk);
    msg_valid  <= 1'b0;
endtask

// Test 1: Add bid → best_bid updates
initial begin
    rst_n     <= 0;
    msg_valid <= 0;
    repeat(3) @(posedge clk);
    rst_n <= 1;
    repeat(2) @(posedge clk);

    $display("\n[TEST 1] Add bid order → best_bid updates");
    send_msg(8'h41, 1'b0, 64'h1, 32'd500, 32'd1824000); // 'A', bid, $182.40
    @(posedge clk);
    tests_run++;
    if (best_bid == 32'd1824000) begin
        $display("  PASS: best_bid = %0d ($182.40)", best_bid);
        tests_passed++;
    end else
        $display("  FAIL: best_bid = %0d (expected 1824000)", best_bid);

    $display("\n[TEST 2] Add ask order → spread and mid_price");
    send_msg(8'h41, 1'b1, 64'h2, 32'd300, 32'd1826000); // 'A', ask, $182.60
    @(posedge clk);
    tests_run++;
    if (best_ask == 32'd1826000 && spread == 32'd2000 && mid_price == 32'd1825000) begin
        $display("  PASS: ask=%0d spread=%0d mid=%0d", best_ask, spread, mid_price);
        tests_passed++;
    end else
        $display("  FAIL: ask=%0d spread=%0d mid=%0d", best_ask, spread, mid_price);

    $display("\n[TEST 3] book_valid asserted when both sides present");
    tests_run++;
    if (book_valid) begin
        $display("  PASS: book_valid = 1");
        tests_passed++;
    end else
        $display("  FAIL: book_valid should be 1");

    $display("\n=== ORDER BOOK TB: %0d / %0d tests passed ===", tests_passed, tests_run);
    $finish;
end

initial begin
    $dumpfile("order_book_tb.vcd");
    $dumpvars(0, order_book_tb);
end

initial begin
    #2000;
    $display("TIMEOUT");
    $finish;
end

endmodule
