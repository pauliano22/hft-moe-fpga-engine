#!/usr/bin/env python3
"""
plot_resource_util.py — FPGA Resource Utilization Bar Chart

Parses a Vivado implementation utilization report (or uses demo data)
and generates a bar chart showing LUT/FF/BRAM/DSP utilization percentages.

Usage:
    # With real Vivado report:
    python3 sim/scripts/plot_resource_util.py --report docs/lob_synthesis.rpt

    # Demo mode (no Vivado needed):
    python3 sim/scripts/plot_resource_util.py

Output: docs/resource_util.png

Target device: xcvu9p-flga2104-2-i (Xilinx UltraScale+)
  LUTs:  1,182,240 available
  FFs:   2,364,480 available
  BRAM:  2,160     available (36Kb blocks)
  DSPs:  6,840     available
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# xcvu9p device resources
DEVICE_RESOURCES = {
    "LUT":   1182240,
    "FF":    2364480,
    "BRAM":  2160,
    "DSP":   6840,
}

def parse_vivado_report(report_path: str) -> dict:
    """
    Parse a Vivado utilization report to extract used resources.
    Looks for lines like:
      | CLB LUTs            |   12345  |     ...
      | CLB Registers       |   67890  |     ...
      | Block RAM Tile      |     32   |     ...
      | DSPs                |    144   |     ...
    """
    used = {}
    with open(report_path) as f:
        for line in f:
            # LUT
            if re.search(r"CLB LUTs\s*\|", line, re.IGNORECASE):
                m = re.search(r"\|\s*(\d+)\s*\|", line)
                if m: used["LUT"] = int(m.group(1))
            # FF
            elif re.search(r"CLB Registers\s*\|", line, re.IGNORECASE):
                m = re.search(r"\|\s*(\d+)\s*\|", line)
                if m: used["FF"] = int(m.group(1))
            # BRAM
            elif re.search(r"Block RAM Tile\s*\|", line, re.IGNORECASE):
                m = re.search(r"\|\s*(\d+)\s*\|", line)
                if m: used["BRAM"] = int(m.group(1))
            # DSP
            elif re.search(r"DSPs\s*\|", line, re.IGNORECASE):
                m = re.search(r"\|\s*(\d+)\s*\|", line)
                if m: used["DSP"] = int(m.group(1))
    return used

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", default=None,
                        help="Path to Vivado utilization report")
    parser.add_argument("--out",    default=os.path.join(ROOT, "docs", "resource_util.png"))
    args = parser.parse_args()

    # Determine resource usage
    if args.report and os.path.exists(args.report):
        used = parse_vivado_report(args.report)
        source = os.path.basename(args.report)
        print(f"Parsed Vivado report: {args.report}")
    else:
        # Demo data — estimated from architecture:
        # LOB:     bid/ask arrays (2×2048×32b = 128KB FF) + order tables
        # Router:  8×4 weight matrix in LUTs + DSPs for multiplies
        # Expert:  2×(8×16×16b + 16×1×16b) = 2×144 DSP multiplies
        print("Using estimated resource data (no Vivado report found)")
        used = {
            "LUT":  145000,  # ~12%: LOB combinatorial logic + gating network
            "FF":   260000,  # ~11%: ARRAY_PARTITION bid/ask registers
            "BRAM": 32,      # ~1.5%: order lookup tables
            "DSP":  320,     # ~5%:   MoE multiplications (2 experts × 144 DSPs)
        }
        source = "estimated (pre-synthesis)"

    if not used:
        print("No resource data found — check report format")
        sys.exit(1)

    # Compute percentages
    pct = {res: (used.get(res, 0) / avail * 100)
           for res, avail in DEVICE_RESOURCES.items()}

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("ERROR: pip3 install matplotlib numpy")
        sys.exit(1)

    resources = list(DEVICE_RESOURCES.keys())
    values    = [pct[r] for r in resources]
    colors    = ["#2196F3", "#4CAF50", "#FF9800", "#9C27B0"]

    fig, ax = plt.subplots(figsize=(9, 5))
    fig.patch.set_facecolor("white")

    bars = ax.barh(resources, values, color=colors, edgecolor="white", height=0.5)

    # Add value labels
    for bar, res, val in zip(bars, resources, values):
        ax.text(val + 0.3, bar.get_y() + bar.get_height()/2,
                f"{val:.1f}%  ({used.get(res, 0):,} / {DEVICE_RESOURCES[res]:,})",
                va="center", fontsize=10)

    # 50% and 80% reference lines
    ax.axvline(50, color="orange", linestyle="--", linewidth=1.5, alpha=0.7,
               label="50% (target ceiling)")
    ax.axvline(80, color="red",    linestyle="--", linewidth=1.5, alpha=0.7,
               label="80% (hard limit)")

    ax.set_xlabel("Utilization (%)", fontsize=12)
    ax.set_title(f"FPGA Resource Utilization — xcvu9p\n"
                 f"Source: {source}", fontsize=13, fontweight="bold")
    ax.set_xlim(0, 100)
    ax.legend(fontsize=9, loc="lower right")
    ax.grid(axis="x", alpha=0.3)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    plt.tight_layout()
    plt.savefig(args.out, dpi=150, bbox_inches="tight")
    print(f"Saved: {args.out}")

if __name__ == "__main__":
    main()
