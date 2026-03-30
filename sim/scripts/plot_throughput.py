#!/usr/bin/env python3
"""
plot_throughput.py — Throughput vs. Latency scatter plot

Reads sim/results/throughput_vs_latency.csv and plots
throughput (M msg/s) vs. p99 latency (ns).

Usage:
    python3 sim/scripts/plot_throughput.py
Output: docs/throughput.png
"""

import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def main():
    csv_path = os.path.join(ROOT, "sim", "results", "throughput_vs_latency.csv")
    out_path = os.path.join(ROOT, "docs", "throughput.png")

    if not os.path.exists(csv_path):
        print(f"WARNING: {csv_path} not found — using synthetic demo data")
        rows = [
            {"idle_cycles": 0, "throughput_Mmps": 150.0, "p99_ns": 22.0, "p50_ns": 20.0},
            {"idle_cycles": 1, "throughput_Mmps": 100.0, "p99_ns": 26.0, "p50_ns": 24.0},
            {"idle_cycles": 2, "throughput_Mmps":  70.0, "p99_ns": 28.0, "p50_ns": 26.0},
            {"idle_cycles": 4, "throughput_Mmps":  45.0, "p99_ns": 36.0, "p50_ns": 32.0},
            {"idle_cycles": 8, "throughput_Mmps":  25.0, "p99_ns": 52.0, "p50_ns": 44.0},
        ]
    else:
        rows = []
        with open(csv_path) as f:
            reader = csv.DictReader(f)
            for row in reader:
                if row.get("throughput_Mmps") and row["throughput_Mmps"] != "None":
                    rows.append({
                        "idle_cycles":    int(row["idle_cycles"]),
                        "throughput_Mmps": float(row["throughput_Mmps"]),
                        "p99_ns":         float(row["p99_ns"]),
                        "p50_ns":         float(row["p50_ns"]),
                    })

    if not rows:
        print("No data to plot")
        sys.exit(0)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("ERROR: pip3 install matplotlib")
        sys.exit(1)

    throughputs = [r["throughput_Mmps"] for r in rows]
    p99s        = [r["p99_ns"]         for r in rows]
    p50s        = [r["p50_ns"]         for r in rows]
    idles       = [r["idle_cycles"]    for r in rows]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    fig.patch.set_facecolor("white")

    # Left: throughput vs idle cycles
    ax1.bar([str(i) for i in idles], throughputs, color="#2196F3", edgecolor="white")
    ax1.set_xlabel("Idle Cycles Between Messages", fontsize=12)
    ax1.set_ylabel("Throughput (M messages/sec)", fontsize=12)
    ax1.set_title("Throughput vs. Injection Rate", fontsize=13, fontweight="bold")
    ax1.axhline(150, color="red", linestyle="--", linewidth=1.5, label="Target 150M msg/s")
    ax1.legend(fontsize=10)
    ax1.grid(axis="y", alpha=0.3)
    for bar, val in zip(ax1.patches, throughputs):
        ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                 f"{val:.0f}", ha="center", fontsize=9)

    # Right: p50 and p99 latency vs idle cycles
    x = range(len(idles))
    ax2.plot([str(i) for i in idles], p50s, "o-", color="#4CAF50", linewidth=2,
             markersize=8, label="p50 latency")
    ax2.plot([str(i) for i in idles], p99s, "s-", color="#F44336", linewidth=2,
             markersize=8, label="p99 latency")
    ax2.axhline(100, color="orange", linestyle="--", linewidth=1.5,
                label="Target <100 ns")
    ax2.set_xlabel("Idle Cycles Between Messages", fontsize=12)
    ax2.set_ylabel("Latency (ns)", fontsize=12)
    ax2.set_title("Latency vs. Injection Rate", fontsize=13, fontweight="bold")
    ax2.legend(fontsize=10)
    ax2.grid(alpha=0.3)

    fig.suptitle("FPGA HFT Engine — Throughput & Latency\n"
                 "(Verilator cycle-accurate simulation @ 250 MHz)",
                 fontsize=13, fontweight="bold", y=1.02)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")

if __name__ == "__main__":
    main()
