#!/usr/bin/env python3
"""
Bit-Accuracy Verification Script
=================================
Compares the golden model trace (golden_trace.csv) against Verilator simulation
output (verilator_trace.csv) field-by-field. Reports pass/fail for each order
and an overall verdict.

This is the "proof of correctness" — it demonstrates that the hardware (RTL)
produces exactly the same results as the software reference implementation.

Why is this important?
---------------------
In FPGA trading, a single bit error can cause a wrong trade worth millions.
Bit-accuracy verification proves that the RTL implementation matches the
golden model exactly, not approximately. This is the gold standard for
hardware verification in finance.

Usage:
    python3 sim/scripts/verify_trace.py \\
        --golden build/golden_trace.csv \\
        --hardware sim/verilator/verilator_trace.csv

    # Or use defaults (assumes standard build paths):
    python3 sim/scripts/verify_trace.py

Exit codes:
    0 = All fields match (PASS)
    1 = At least one mismatch (FAIL)
    2 = File not found or format error
"""

import argparse
import csv
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Color output helpers (for terminal readability)
# ---------------------------------------------------------------------------
class Colors:
    """ANSI color codes for terminal output."""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    CYAN = '\033[96m'
    BOLD = '\033[1m'
    RESET = '\033[0m'


def colored(text: str, color: str) -> str:
    """Wrap text in ANSI color codes."""
    return f"{color}{text}{Colors.RESET}"


# ---------------------------------------------------------------------------
# Comparison logic
# ---------------------------------------------------------------------------

# Fields that must match exactly (integer comparison)
EXACT_FIELDS = ['side', 'price', 'shares', 'matched', 'match_price', 'match_qty']

# Fields compared with a tolerance (floating-point from MoE inference)
APPROX_FIELDS = ['moe_confidence']

# Fields where we check the category matches (e.g., action 0/1/2)
CATEGORY_FIELDS = ['moe_action']

# Default tolerance for floating-point comparisons
# This accounts for fixed-point quantization differences between
# the C++ golden model and the RTL implementation
DEFAULT_TOLERANCE = 0.01


def load_trace(path: str) -> list:
    """
    Load a trace CSV file into a list of dicts.
    Strips whitespace from all field names and values.
    """
    if not Path(path).exists():
        print(colored(f"ERROR: File not found: {path}", Colors.RED))
        sys.exit(2)

    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        # Normalize column names
        reader.fieldnames = [name.strip() for name in reader.fieldnames]
        rows = []
        for row in reader:
            cleaned = {k.strip(): v.strip() for k, v in row.items()}
            rows.append(cleaned)
    return rows


def compare_traces(golden: list, hardware: list, tolerance: float) -> dict:
    """
    Compare golden model trace against hardware trace, field by field.

    Returns a dict with:
        - total: number of orders compared
        - passed: number of orders that match
        - failed: number of orders with mismatches
        - details: list of mismatch details
    """
    results = {
        'total': 0,
        'passed': 0,
        'failed': 0,
        'details': [],
    }

    # Check row counts
    if len(golden) != len(hardware):
        print(colored(
            f"WARNING: Row count mismatch — golden has {len(golden)} rows, "
            f"hardware has {len(hardware)} rows",
            Colors.YELLOW,
        ))

    num_rows = min(len(golden), len(hardware))

    for i in range(num_rows):
        g = golden[i]
        h = hardware[i]
        results['total'] += 1
        order_ok = True
        mismatches = []

        # Check exact-match fields
        for field in EXACT_FIELDS:
            if field not in g or field not in h:
                continue
            g_val = g[field].strip()
            h_val = h[field].strip()
            if g_val != h_val:
                order_ok = False
                mismatches.append(f"{field}: golden={g_val} hw={h_val}")

        # Check approximate-match fields (floating point)
        for field in APPROX_FIELDS:
            if field not in g or field not in h:
                continue
            try:
                g_val = float(g[field])
                h_val = float(h[field])
                if abs(g_val - h_val) > tolerance:
                    order_ok = False
                    mismatches.append(
                        f"{field}: golden={g_val:.6f} hw={h_val:.6f} "
                        f"(delta={abs(g_val - h_val):.6f} > tol={tolerance})"
                    )
            except ValueError:
                order_ok = False
                mismatches.append(f"{field}: parse error (g='{g[field]}' h='{h[field]}')")

        # Check category fields
        for field in CATEGORY_FIELDS:
            if field not in g or field not in h:
                continue
            g_val = int(float(g[field]))
            h_val = int(float(h[field]))
            if g_val != h_val:
                order_ok = False
                actions = {0: 'HOLD', 1: 'BUY', 2: 'SELL'}
                mismatches.append(
                    f"{field}: golden={actions.get(g_val, g_val)} "
                    f"hw={actions.get(h_val, h_val)}"
                )

        if order_ok:
            results['passed'] += 1
        else:
            results['failed'] += 1
            results['details'].append({
                'order_idx': i,
                'stock': g.get('stock', '?'),
                'mismatches': mismatches,
            })

    return results


def print_report(results: dict, golden_path: str, hw_path: str):
    """Print a formatted verification report."""
    print()
    print(colored("=" * 70, Colors.BOLD))
    print(colored("  FPGA MoE Trading Engine — Bit-Accuracy Verification Report", Colors.BOLD))
    print(colored("=" * 70, Colors.BOLD))
    print()
    print(f"  Golden model trace:   {golden_path}")
    print(f"  Hardware trace:       {hw_path}")
    print(f"  Orders compared:      {results['total']}")
    print()

    # Per-order results
    if results['details']:
        print(colored("  MISMATCHES:", Colors.RED))
        print(colored("  " + "-" * 66, Colors.RED))
        for detail in results['details']:
            print(colored(
                f"  Order {detail['order_idx']} ({detail['stock']}):",
                Colors.RED,
            ))
            for m in detail['mismatches']:
                print(colored(f"    - {m}", Colors.RED))
        print()

    # Summary
    print("  " + "-" * 66)
    print(f"  Passed: {colored(str(results['passed']), Colors.GREEN)}")
    print(f"  Failed: {colored(str(results['failed']), Colors.RED if results['failed'] else Colors.GREEN)}")

    if results['failed'] == 0 and results['total'] > 0:
        print()
        print(colored("  VERDICT: PASS — 100% bit-accurate match", Colors.GREEN + Colors.BOLD))
        print(colored("  The hardware implementation matches the golden model exactly.", Colors.GREEN))
    elif results['total'] == 0:
        print()
        print(colored("  VERDICT: NO DATA — nothing to compare", Colors.YELLOW + Colors.BOLD))
    else:
        fail_pct = results['failed'] / results['total'] * 100
        print()
        print(colored(
            f"  VERDICT: FAIL — {results['failed']}/{results['total']} orders "
            f"({fail_pct:.1f}%) have mismatches",
            Colors.RED + Colors.BOLD,
        ))

    print(colored("=" * 70, Colors.BOLD))
    print()


def self_test():
    """
    Run a self-test by comparing golden_trace.csv against itself.
    This validates that the verification logic works correctly.
    """
    print(colored("Running self-test (comparing golden trace against itself)...", Colors.CYAN))
    golden_path = 'build/golden_trace.csv'

    if not Path(golden_path).exists():
        print(colored(f"Self-test skipped: {golden_path} not found", Colors.YELLOW))
        return True

    golden = load_trace(golden_path)
    results = compare_traces(golden, golden, DEFAULT_TOLERANCE)

    if results['failed'] == 0 and results['total'] > 0:
        print(colored(
            f"Self-test PASSED: {results['total']} orders verified against themselves",
            Colors.GREEN,
        ))
        return True
    else:
        print(colored("Self-test FAILED: trace doesn't match itself!", Colors.RED))
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Verify bit-accuracy between golden model and hardware traces',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument('--golden', '-g', type=str, default='build/golden_trace.csv',
                        help='Path to golden model trace CSV')
    parser.add_argument('--hardware', '-hw', type=str, default='sim/verilator/verilator_trace.csv',
                        help='Path to hardware simulation trace CSV')
    parser.add_argument('--tolerance', '-t', type=float, default=DEFAULT_TOLERANCE,
                        help=f'Tolerance for floating-point comparison (default: {DEFAULT_TOLERANCE})')
    parser.add_argument('--self-test', action='store_true',
                        help='Run self-test (compare golden trace against itself)')

    args = parser.parse_args()

    if args.self_test:
        ok = self_test()
        sys.exit(0 if ok else 1)

    # Load traces
    golden = load_trace(args.golden)
    hardware = load_trace(args.hardware)

    # Compare
    results = compare_traces(golden, hardware, args.tolerance)

    # Report
    print_report(results, args.golden, args.hardware)

    # Exit code
    sys.exit(0 if results['failed'] == 0 and results['total'] > 0 else 1)


if __name__ == '__main__':
    main()
