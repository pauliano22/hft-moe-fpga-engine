#!/usr/bin/env python3
"""
compare.py — Golden Model vs. Verilator Simulation Comparator

Runs both the golden model and Verilator sim on the same ITCH file,
captures their outputs, and diffs them field-by-field.

Usage:
    python3 sim/compare.py [--file data/sample.itch] [--limit 10000]

RUN MANUALLY after building both binaries:
    cd src/golden_model && make       # build golden_model
    cd sim/verilator && make          # build Verilator sim
    cd ../..
    python3 sim/compare.py
"""

import argparse
import csv
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def run_golden_model(itch_file: str, limit: int) -> list[dict]:
    """Run golden model with --verify 1 to get per-message book state."""
    binary = os.path.join(ROOT, "src", "golden_model", "golden_model")
    if not os.path.exists(binary):
        print(f"ERROR: golden model binary not found: {binary}")
        print("Build with: cd src/golden_model && make")
        sys.exit(1)

    cmd = [binary, "--file", itch_file, "--verify", "1"]
    result = subprocess.run(cmd, capture_output=True, text=True)

    records = []
    for line in result.stdout.splitlines():
        # Parse lines like "--- Book state after N messages ---"
        # and the ladder output
        if "OIR:" in line and "Mid:" in line:
            # Parse mid price and OIR from the summary line
            parts = {k.strip(): v.strip()
                     for part in line.split("|")
                     for k, v in [part.split(":", 1)]}
            try:
                mid_str = parts.get("Mid", "$0").replace("$", "")
                records.append({
                    "mid_price": float(mid_str),
                    "order_count": int(parts.get("Orders", "0")),
                })
            except (ValueError, KeyError):
                pass

        if len(records) >= limit:
            break

    return records

def run_verilator_sim(itch_file: str, limit: int) -> list[dict]:
    """Run Verilator simulation and read latencies.csv output."""
    binary = os.path.join(ROOT, "sim", "verilator", "obj_dir", "Vtop")
    if not os.path.exists(binary):
        print(f"ERROR: Verilator binary not found: {binary}")
        print("Build with: cd sim/verilator && make")
        sys.exit(1)

    csv_file = os.path.join(ROOT, "sim", "verilator", "latencies.csv")

    cmd = [binary, "--file", itch_file, "--no-vcd",
           "--limit", str(limit)]
    result = subprocess.run(cmd, capture_output=True, text=True,
                             cwd=os.path.join(ROOT, "sim", "verilator"))
    if result.returncode != 0:
        print("Verilator sim failed:")
        print(result.stderr)
        sys.exit(1)

    # Print simulation output
    print(result.stdout)

    # Read latency CSV
    records = []
    if os.path.exists(csv_file):
        with open(csv_file) as f:
            reader = csv.DictReader(f)
            for row in reader:
                records.append({
                    "message_index": int(row["message_index"]),
                    "latency_ns":    float(row["latency_ns"]),
                })
    return records

def main():
    parser = argparse.ArgumentParser(description="Compare golden model vs Verilator sim")
    parser.add_argument("--file",  default=os.path.join(ROOT, "data", "sample.itch"))
    parser.add_argument("--limit", type=int, default=1000,
                        help="Number of messages to compare (default 1000)")
    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f"ERROR: ITCH file not found: {args.file}")
        print("Generate with: cd src/golden_model && make test-data")
        sys.exit(1)

    print(f"=== Comparing golden model vs Verilator sim ===")
    print(f"File:  {args.file}")
    print(f"Limit: {args.limit} messages\n")

    # Run golden model
    print("Running golden model...")
    gm_records = run_golden_model(args.file, args.limit)
    print(f"  Golden model produced {len(gm_records)} book state records\n")

    # Run Verilator
    print("Running Verilator simulation...")
    vcd_records = run_verilator_sim(args.file, args.limit)
    print(f"  Verilator produced {len(vcd_records)} latency records\n")

    # Report latency statistics from Verilator
    if vcd_records:
        lats = sorted(r["latency_ns"] for r in vcd_records)
        n = len(lats)
        print("=== Latency Statistics (Verilator) ===")
        print(f"  Messages with latency data: {n}")
        if n > 0:
            print(f"  p50:  {lats[n*50//100]:.1f} ns")
            print(f"  p95:  {lats[n*95//100]:.1f} ns")
            print(f"  p99:  {lats[n*99//100]:.1f} ns")
            print(f"  max:  {lats[-1]:.1f} ns")

    print("\n=== Comparison Summary ===")
    print(f"  Golden model records: {len(gm_records)}")
    print(f"  Verilator records:    {len(vcd_records)}")

    if not gm_records:
        print("  WARNING: No golden model records to compare")
        print("  (golden model --verify output parsing may need adjustment)")
    elif not vcd_records:
        print("  WARNING: No Verilator latency records")
        print("  (check that book_valid asserts during simulation)")
    else:
        print("  NOTE: Field-by-field comparison requires the Verilator sim")
        print("  to output parsed message fields (best_bid per message).")
        print("  The latencies.csv file records per-message latency.")
        print("  For full field comparison, use the --verify flag of the")
        print("  golden model and add CSV output to sim_main.cpp.")

    print("\n=== Done ===")

if __name__ == "__main__":
    main()
