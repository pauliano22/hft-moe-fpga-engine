#!/usr/bin/env python3
"""
ITCH 5.0 Test Data Generator
=============================
Generates realistic NASDAQ ITCH 5.0 "Add Order" binary messages from a CSV
of historical prices. This is used to feed the Verilator simulation and the
C++ golden model with data that looks like real market activity.

ITCH 5.0 Add Order message layout (36 bytes, big-endian):
    Offset  Size  Field
    0       1     Message Type ('A' = 0x41)
    1       2     Stock Locate (index into stock directory)
    3       2     Tracking Number
    5       6     Timestamp (nanoseconds since midnight)
    11      8     Order Reference Number (unique per order)
    19      1     Buy/Sell Indicator ('B' = buy, 'S' = sell)
    20      4     Shares (number of shares)
    24      8     Stock Symbol (right-padded with spaces)
    32      4     Price (4 implied decimal places, so $10.00 = 100000)

Usage:
    # Generate from a CSV file:
    python3 tools/generate_itch_data.py --input data/prices.csv --output data/test_orders.bin

    # Generate synthetic data (no CSV needed):
    python3 tools/generate_itch_data.py --synthetic --num-orders 1000 --output data/test_orders.bin

    # Also produce a human-readable CSV for inspection:
    python3 tools/generate_itch_data.py --synthetic --output data/test_orders.bin --csv data/test_orders.csv
"""

import argparse
import csv
import os
import random
import struct
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# ITCH message constants
# ---------------------------------------------------------------------------
ITCH_ADD_ORDER = 0x41          # 'A' — the message type byte
ADD_ORDER_SIZE = 36            # Total bytes per Add Order message
STOCK_FIELD_LEN = 8            # Stock symbol is always 8 ASCII bytes
PRICE_DECIMALS = 4             # ITCH prices have 4 implied decimal places
PRICE_SCALE = 10 ** PRICE_DECIMALS  # Multiply dollar price by this


def pack_add_order(
    timestamp_ns: int,
    order_ref: int,
    side: str,
    shares: int,
    stock: str,
    price_raw: int,
    stock_locate: int = 0,
    tracking_num: int = 0,
) -> bytes:
    """
    Pack a single ITCH 5.0 Add Order message into 36 bytes (big-endian).

    Parameters
    ----------
    timestamp_ns : int
        Nanoseconds since midnight (max 6 bytes = ~281 trillion ns).
    order_ref : int
        Unique order reference number (8 bytes).
    side : str
        'B' for buy, 'S' for sell.
    shares : int
        Number of shares (unsigned 32-bit).
    stock : str
        Ticker symbol, up to 8 characters (right-padded with spaces).
    price_raw : int
        Price in ITCH format (4 implied decimals). $150.25 → 1502500.
    stock_locate : int
        Index into the stock directory (usually 0 for testing).
    tracking_num : int
        Tracking number (usually 0 for testing).

    Returns
    -------
    bytes
        Exactly 36 bytes — one complete ITCH Add Order message.
    """
    # Validate inputs
    assert side in ('B', 'S'), f"Side must be 'B' or 'S', got '{side}'"
    assert 0 <= shares < 2**32, f"Shares out of range: {shares}"
    assert 0 <= price_raw < 2**32, f"Price out of range: {price_raw}"
    assert len(stock) <= STOCK_FIELD_LEN, f"Stock symbol too long: '{stock}'"

    # Right-pad stock symbol with spaces to exactly 8 bytes
    stock_bytes = stock.encode('ascii').ljust(STOCK_FIELD_LEN, b' ')

    # Pack everything in big-endian byte order:
    #   B  = 1 byte  (message type)
    #   H  = 2 bytes (stock locate)
    #   H  = 2 bytes (tracking number)
    #   6s = 6 bytes (timestamp — we use manual packing for 6-byte int)
    #   Q  = 8 bytes (order reference)
    #   c  = 1 byte  (side)
    #   I  = 4 bytes (shares)
    #   8s = 8 bytes (stock)
    #   I  = 4 bytes (price)
    ts_bytes = timestamp_ns.to_bytes(6, byteorder='big')

    msg = struct.pack(
        '>B H H',
        ITCH_ADD_ORDER,
        stock_locate,
        tracking_num,
    )
    msg += ts_bytes
    msg += struct.pack(
        '>Q c I 8s I',
        order_ref,
        side.encode('ascii'),
        shares,
        stock_bytes,
        price_raw,
    )

    assert len(msg) == ADD_ORDER_SIZE, f"Message size {len(msg)} != {ADD_ORDER_SIZE}"
    return msg


def generate_from_csv(csv_path: str, base_timestamp_ns: int = 34_200_000_000_000):
    """
    Read a CSV of historical prices and generate ITCH Add Order messages.

    Expected CSV columns (flexible — uses what's available):
        symbol, price, side, shares

    If 'side' is missing, alternates buy/sell.
    If 'shares' is missing, uses random realistic quantities.

    Parameters
    ----------
    csv_path : str
        Path to the input CSV file.
    base_timestamp_ns : int
        Starting timestamp in nanoseconds. Default is 9:30 AM ET
        (34,200 seconds × 1e9 ns = market open).

    Yields
    ------
    tuple(bytes, dict)
        The packed binary message and a metadata dict for CSV output.
    """
    timestamp_ns = base_timestamp_ns
    order_ref = 1

    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)

        # Normalize column names to lowercase, strip whitespace
        reader.fieldnames = [name.strip().lower() for name in reader.fieldnames]

        for row in reader:
            # Extract symbol (required)
            stock = row.get('symbol', row.get('ticker', row.get('stock', 'AAPL')))
            stock = stock.strip().upper()[:STOCK_FIELD_LEN]

            # Extract price (required) — convert dollars to ITCH format
            price_str = row.get('price', row.get('close', row.get('last', '100.0')))
            price_dollars = float(price_str)
            price_raw = int(round(price_dollars * PRICE_SCALE))

            # Extract side (optional — alternate if missing)
            side = row.get('side', '').strip().upper()
            if side not in ('B', 'S'):
                side = 'B' if order_ref % 2 == 1 else 'S'

            # Extract shares (optional — random if missing)
            shares_str = row.get('shares', row.get('volume', row.get('qty', '')))
            if shares_str and shares_str.strip():
                shares = int(float(shares_str.strip()))
            else:
                shares = random.choice([100, 200, 300, 500, 1000])

            # Advance timestamp by a realistic inter-arrival time
            # Real ITCH messages arrive ~100ns-10μs apart during active trading
            timestamp_ns += random.randint(100, 10_000)

            msg = pack_add_order(
                timestamp_ns=timestamp_ns,
                order_ref=order_ref,
                side=side,
                shares=shares,
                stock=stock,
                price_raw=price_raw,
            )

            metadata = {
                'order_ref': order_ref,
                'timestamp_ns': timestamp_ns,
                'side': side,
                'shares': shares,
                'stock': stock,
                'price_raw': price_raw,
                'price_dollars': price_dollars,
            }

            yield msg, metadata
            order_ref += 1


def generate_synthetic(
    num_orders: int = 100,
    symbols: list = None,
    base_price: float = 150.0,
    volatility: float = 0.001,
    base_timestamp_ns: int = 34_200_000_000_000,
):
    """
    Generate synthetic but realistic ITCH order flow using a random walk.

    This simulates what a real trading day looks like: prices drift around
    a base price with small random changes (geometric Brownian motion),
    buy and sell orders arrive at realistic intervals, and order sizes
    follow typical distributions.

    Parameters
    ----------
    num_orders : int
        How many orders to generate.
    symbols : list
        List of ticker symbols to include.
    base_price : float
        Starting price in dollars.
    volatility : float
        Per-tick price change standard deviation (fraction of price).
    base_timestamp_ns : int
        Starting nanosecond timestamp (default: 9:30 AM market open).

    Yields
    ------
    tuple(bytes, dict)
        Packed binary message and metadata dict.
    """
    if symbols is None:
        symbols = ['AAPL', 'GOOG', 'MSFT', 'TSLA', 'AMZN', 'META', 'NVDA', 'AMD']

    # Track a mid-price per symbol (simulates price discovery)
    prices = {sym: base_price for sym in symbols}
    timestamp_ns = base_timestamp_ns
    order_ref = 1

    # Realistic lot sizes (round lots are most common)
    lot_sizes = [100, 100, 100, 200, 200, 300, 500, 1000]

    for i in range(num_orders):
        # Pick a random symbol
        stock = random.choice(symbols)

        # Random walk the price (geometric Brownian motion)
        # This creates realistic price series where changes are proportional
        # to the current price level
        price_change = random.gauss(0, volatility) * prices[stock]
        prices[stock] = max(1.0, prices[stock] + price_change)

        # Decide buy vs sell (slightly biased by recent price movement)
        # If price went up, more likely to see sells (profit-taking)
        # If price went down, more likely to see buys (bargain-hunting)
        buy_probability = 0.5 - (price_change / prices[stock]) * 10
        buy_probability = max(0.3, min(0.7, buy_probability))
        side = 'B' if random.random() < buy_probability else 'S'

        # Price offset from mid: buys below mid, sells above mid
        # This creates a realistic bid-ask spread
        spread_ticks = random.randint(1, 10)  # 1-10 ticks of spread
        if side == 'B':
            order_price = prices[stock] - spread_ticks * 0.01
        else:
            order_price = prices[stock] + spread_ticks * 0.01

        price_raw = int(round(order_price * PRICE_SCALE))
        shares = random.choice(lot_sizes)

        # Realistic inter-arrival: bursts (100ns) and quiet periods (10μs)
        if random.random() < 0.2:
            # Burst: very fast arrivals (100-500 ns)
            timestamp_ns += random.randint(100, 500)
        else:
            # Normal: typical gap (1μs-10μs)
            timestamp_ns += random.randint(1_000, 10_000)

        msg = pack_add_order(
            timestamp_ns=timestamp_ns,
            order_ref=order_ref,
            side=side,
            shares=shares,
            stock=stock,
            price_raw=price_raw,
        )

        metadata = {
            'order_ref': order_ref,
            'timestamp_ns': timestamp_ns,
            'side': side,
            'shares': shares,
            'stock': stock,
            'price_raw': price_raw,
            'price_dollars': round(order_price, 4),
        }

        yield msg, metadata
        order_ref += 1


def write_output(orders, bin_path: str, csv_path: str = None):
    """Write generated orders to binary and optionally CSV files."""
    count = 0
    csv_writer = None
    csv_file = None

    if csv_path:
        csv_file = open(csv_path, 'w', newline='')
        csv_writer = csv.DictWriter(csv_file, fieldnames=[
            'order_ref', 'timestamp_ns', 'side', 'shares',
            'stock', 'price_raw', 'price_dollars',
        ])
        csv_writer.writeheader()

    with open(bin_path, 'wb') as bf:
        for msg_bytes, metadata in orders:
            bf.write(msg_bytes)
            if csv_writer:
                csv_writer.writerow(metadata)
            count += 1

    if csv_file:
        csv_file.close()

    return count


def main():
    parser = argparse.ArgumentParser(
        description='Generate ITCH 5.0 Add Order test data',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # From a CSV of historical prices:
  %(prog)s --input data/prices.csv --output data/test_orders.bin

  # Synthetic random walk (no CSV needed):
  %(prog)s --synthetic --num-orders 1000 --output data/test_orders.bin

  # With a human-readable CSV alongside:
  %(prog)s --synthetic --output data/test_orders.bin --csv data/test_orders.csv
        """,
    )

    parser.add_argument('--input', '-i', type=str,
                        help='Path to CSV file with historical prices')
    parser.add_argument('--output', '-o', type=str, default='data/test_orders.bin',
                        help='Output binary file path (default: data/test_orders.bin)')
    parser.add_argument('--csv', type=str, default=None,
                        help='Also write a human-readable CSV (optional)')
    parser.add_argument('--synthetic', action='store_true',
                        help='Generate synthetic data instead of reading CSV')
    parser.add_argument('--num-orders', type=int, default=1000,
                        help='Number of synthetic orders to generate (default: 1000)')
    parser.add_argument('--symbols', type=str, nargs='+',
                        default=None,
                        help='Ticker symbols for synthetic data')
    parser.add_argument('--base-price', type=float, default=150.0,
                        help='Starting price for synthetic data (default: $150.00)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed for reproducibility (default: 42)')

    args = parser.parse_args()

    # Seed for reproducibility — important for regression testing
    random.seed(args.seed)

    # Validate: must specify either --input or --synthetic
    if not args.input and not args.synthetic:
        parser.error('Must specify either --input (CSV file) or --synthetic')

    # Ensure output directory exists
    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    if args.csv:
        os.makedirs(os.path.dirname(args.csv) or '.', exist_ok=True)

    # Generate orders
    if args.synthetic:
        print(f"Generating {args.num_orders} synthetic ITCH orders...")
        orders = generate_synthetic(
            num_orders=args.num_orders,
            symbols=args.symbols,
            base_price=args.base_price,
        )
    else:
        if not Path(args.input).exists():
            print(f"Error: Input file not found: {args.input}", file=sys.stderr)
            sys.exit(1)
        print(f"Generating ITCH orders from CSV: {args.input}")
        orders = generate_from_csv(args.input)

    # Write output
    count = write_output(orders, args.output, args.csv)

    # Summary
    file_size = os.path.getsize(args.output)
    print(f"Generated {count} orders ({file_size:,} bytes) → {args.output}")
    if args.csv:
        print(f"CSV log → {args.csv}")
    print(f"Each message: {ADD_ORDER_SIZE} bytes (ITCH Add Order format)")


if __name__ == '__main__':
    main()
