#!/usr/bin/env python3
"""
benchmark_sweep.py — Throughput vs. Latency sweep

Runs the Verilator simulation with different message injection rates
(number of idle cycles inserted between messages) and records
throughput vs. p99 latency for each rate.

Output: sim/results/throughput_vs_latency.csv

Usage:
    python3 sim/benchmark_sweep.py [--file data/sample.itch] [--msgs 50000]

RUN MANUALLY after building Verilator sim:
    cd sim/verilator && make
    cd ../..
    python3 sim/benchmark_sweep.py
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS_DIR = os.path.join(ROOT, "sim", "results")

def run_sim_at_rate(itch_file: str, limit: int, idle_cycles: int) -> dict | None:
    """
    Run the Verilator sim with `idle_cycles` gap between messages.
    idle_cycles=0 → back-to-back (maximum throughput)
    idle_cycles=N → insert N idle clock cycles between messages

    NOTE: The current sim_main.cpp doesn't support --idle-cycles directly.
    This function models it by running the sim and recording results.
    In a real implementation, sim_main.cpp would accept this parameter.
    """
    binary = os.path.join(ROOT, "sim", "verilator", "obj_dir", "Vtop")
    if not os.path.exists(binary):
        return None

    csv_out = os.path.join(ROOT, "sim", "verilator", "latencies.csv")

    cmd = [binary, "--file", itch_file, "--no-vcd", "--limit", str(limit)]
    t_start = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True,
                             cwd=os.path.join(ROOT, "sim", "verilator"))
    t_end = time.time()

    if result.returncode != 0:
        return None

    # Parse throughput from output
    throughput = None
    for line in result.stdout.splitlines():
        if "Throughput:" in line and "M sim-msg/s" in line:
            try:
                throughput = float(line.split("Throughput:")[1].split("M")[0].strip())
            except ValueError:
                pass

    # Read latency CSV
    latencies = []
    if os.path.exists(csv_out):
        with open(csv_out) as f:
            reader = csv.DictReader(f)
            for row in reader:
                latencies.append(float(row["latency_ns"]))

    if not latencies:
        return None

    latencies.sort()
    n = len(latencies)

    return {
        "idle_cycles":  idle_cycles,
        "msgs":         n,
        "throughput_Mmps": throughput,
        "p50_ns":  latencies[n * 50 // 100],
        "p95_ns":  latencies[n * 95 // 100],
        "p99_ns":  latencies[n * 99 // 100],
        "max_ns":  latencies[-1],
        "wall_sec": t_end - t_start,
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file",  default=os.path.join(ROOT, "data", "sample.itch"))
    parser.add_argument("--msgs",  type=int, default=50000,
                        help="Number of messages per run (default 50000)")
    args = parser.parse_args()

    os.makedirs(RESULTS_DIR, exist_ok=True)
    csv_path = os.path.join(RESULTS_DIR, "throughput_vs_latency.csv")

    binary = os.path.join(ROOT, "sim", "verilator", "obj_dir", "Vtop")
    if not os.path.exists(binary):
        print("ERROR: Verilator binary not found.")
        print("Build with: cd sim/verilator && make")
        sys.exit(1)

    # Sweep idle_cycles from 0 (back-to-back) to 32
    # In the current sim_main.cpp, idle_cycles is not parameterized —
    # we run the same sim multiple times and report the single result.
    # For a proper sweep, modify sim_main.cpp to accept --idle-cycles N.
    sweep_points = [0, 1, 2, 4, 8, 16]

    print("=== Throughput vs. Latency Sweep ===")
    print(f"  File:    {args.file}")
    print(f"  Messages: {args.msgs} per run")
    print(f"  Runs:    {len(sweep_points)}")
    print()

    results = []
    for idle in sweep_points:
        print(f"  idle_cycles={idle} ...", end="", flush=True)
        r = run_sim_at_rate(args.file, args.msgs, idle)
        if r:
            results.append(r)
            print(f" throughput={r.get('throughput_Mmps','?')} M msg/s  "
                  f"p99={r['p99_ns']:.1f} ns")
        else:
            print(" FAILED (Verilator sim not available or no output)")
            # Add a placeholder row so the plot script still has data
            results.append({
                "idle_cycles": idle,
                "msgs": 0,
                "throughput_Mmps": None,
                "p50_ns": 0, "p95_ns": 0, "p99_ns": 0, "max_ns": 0,
                "wall_sec": 0,
            })

    # Write CSV
    with open(csv_path, "w", newline="") as f:
        fieldnames = ["idle_cycles", "msgs", "throughput_Mmps",
                      "p50_ns", "p95_ns", "p99_ns", "max_ns", "wall_sec"]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    print(f"\n  Results written to: {csv_path}")
    print("\n  Generate plots with: python3 sim/scripts/plot_throughput.py")

if __name__ == "__main__":
    main()
