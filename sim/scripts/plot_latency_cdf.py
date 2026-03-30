#!/usr/bin/env python3
"""
plot_latency_cdf.py — Latency CDF plot (log-scale x-axis)

Reads sim/verilator/latencies.csv and generates a CDF plot showing
the distribution of wire-to-response latencies.

Usage:
    python3 sim/scripts/plot_latency_cdf.py [--csv sim/verilator/latencies.csv]

Output: docs/latency_cdf.png

Install matplotlib if needed:
    pip3 install matplotlib
"""

import argparse
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", default=os.path.join(ROOT, "sim", "verilator", "latencies.csv"))
    parser.add_argument("--out", default=os.path.join(ROOT, "docs", "latency_cdf.png"))
    args = parser.parse_args()

    # Load latency data
    if not os.path.exists(args.csv):
        # Generate synthetic data for demonstration if real data unavailable
        print(f"WARNING: {args.csv} not found — using synthetic demo data")
        import random
        random.seed(42)
        # Simulate realistic latencies: mostly 20-28 ns, occasional outliers
        latencies = [20.0 + random.expovariate(0.5) for _ in range(10000)]
        latencies = [min(max(l, 20.0), 200.0) for l in latencies]
        data_source = "synthetic (demo)"
    else:
        latencies = []
        with open(args.csv) as f:
            reader = csv.DictReader(f)
            for row in reader:
                latencies.append(float(row["latency_ns"]))
        data_source = args.csv

    if not latencies:
        print("ERROR: No latency data found")
        sys.exit(1)

    try:
        import matplotlib
        matplotlib.use("Agg")  # non-interactive backend for server/WSL
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("ERROR: matplotlib not installed. Run: pip3 install matplotlib numpy")
        sys.exit(1)

    latencies = sorted(latencies)
    n = len(latencies)
    cdf = [(i + 1) / n for i in range(n)]

    # Compute percentiles for annotation
    p50  = latencies[int(n * 0.50)]
    p95  = latencies[int(n * 0.95)]
    p99  = latencies[int(n * 0.99)]
    pmax = latencies[-1]

    # --- Plot ---
    fig, ax = plt.subplots(figsize=(10, 6))
    fig.patch.set_facecolor("white")

    ax.plot(latencies, cdf, color="#2196F3", linewidth=2, label="CDF")

    # Annotate key percentiles
    for pct, val, color in [(0.50, p50, "#4CAF50"), (0.95, p95, "#FF9800"),
                             (0.99, p99, "#F44336")]:
        ax.axvline(val, color=color, linestyle="--", linewidth=1.5, alpha=0.8)
        ax.axhline(pct, color=color, linestyle=":",  linewidth=1.0, alpha=0.6)
        ax.annotate(f"p{int(pct*100)}={val:.1f} ns",
                    xy=(val, pct),
                    xytext=(val * 1.05, pct - 0.05),
                    fontsize=9, color=color,
                    arrowprops=dict(arrowstyle="->", color=color, lw=1))

    ax.set_xscale("log")
    ax.set_xlabel("Latency (ns) — log scale", fontsize=12)
    ax.set_ylabel("CDF", fontsize=12)
    ax.set_title("Wire-to-Response Latency CDF\n"
                 "FPGA-Accelerated HFT Engine (Verilator simulation @ 250 MHz)",
                 fontsize=13, fontweight="bold")
    ax.set_xlim(left=max(1, min(latencies) * 0.8))
    ax.set_ylim(0, 1.05)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=10)

    # Stats box
    stats_text = (f"n={n:,} messages\n"
                  f"p50={p50:.1f} ns\n"
                  f"p95={p95:.1f} ns\n"
                  f"p99={p99:.1f} ns\n"
                  f"max={pmax:.1f} ns")
    ax.text(0.97, 0.35, stats_text, transform=ax.transAxes,
            verticalalignment="top", horizontalalignment="right",
            fontsize=9, fontfamily="monospace",
            bbox=dict(boxstyle="round", facecolor="lightyellow", alpha=0.8))

    ax.text(0.97, 0.02, f"Source: {os.path.basename(data_source)}",
            transform=ax.transAxes, fontsize=7, color="gray",
            horizontalalignment="right")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    plt.tight_layout()
    plt.savefig(args.out, dpi=150, bbox_inches="tight")
    print(f"Saved: {args.out}")

if __name__ == "__main__":
    main()
