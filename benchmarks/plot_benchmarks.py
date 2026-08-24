#!/usr/bin/env python3
"""Plot Phase 7 baseline benchmark CSV."""

from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

CSV_PATH = Path("benchmarks/output/phase7_baseline.csv")
OUT_DIR = Path("docs/benchmarks")


def load_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"Missing {path}. Run ./benchmarks/benchmark first.")

    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def plot_read_comparison(rows: list[dict[str, str]]) -> None:
    benchmarks = ["read_hit", "read_miss"]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4))

    for ax, bench in zip(axes, benchmarks):
        subset = [r for r in rows if r["benchmark"] == bench]
        modes = [r["read_mode"] for r in subset]
        latencies = [float(r["avg_latency_us"]) for r in subset]
        bars = ax.bar(modes, latencies, color=["#4C78A8", "#F58518"])
        ax.set_title(bench.replace("_", " "))
        ax.set_ylabel("avg latency (us)")
        ax.set_xlabel("read mode")
        for bar, value in zip(bars, latencies):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                    f"{value:.1f}", ha="center", va="bottom", fontsize=9)

    fig.suptitle("Phase 7: optimized vs linear scan")
    fig.tight_layout()
    out = OUT_DIR / "phase7_read_comparison.png"
    fig.savefig(out, dpi=150)
    print(f"Wrote {out}")


def plot_sstable_sweep(rows: list[dict[str, str]]) -> None:
    subset = [r for r in rows if r["benchmark"] == "read_hit_vs_sstables"]
    by_mode: dict[str, list[tuple[int, float]]] = defaultdict(list)

    for row in subset:
        by_mode[row["read_mode"]].append(
            (int(row["num_sstables"]), float(row["avg_latency_us"]))
        )

    fig, ax = plt.subplots(figsize=(7, 4))
    for mode, points in sorted(by_mode.items()):
        points.sort(key=lambda p: p[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        ax.plot(xs, ys, marker="o", label=mode)

    ax.set_title("Read latency vs SSTable count")
    ax.set_xlabel("number of SSTables")
    ax.set_ylabel("avg latency (us)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    out = OUT_DIR / "phase7_sstable_sweep.png"
    fig.savefig(out, dpi=150)
    print(f"Wrote {out}")


def plot_write_throughput(rows: list[dict[str, str]]) -> None:
    subset = [r for r in rows if r["benchmark"] == "write_throughput"]
    if not subset:
        return

    value = float(subset[0]["ops_per_sec"])
    fig, ax = plt.subplots(figsize=(4, 4))
    ax.bar(["write"], [value], color="#54A24B")
    ax.set_ylabel("puts/sec")
    ax.set_title("Write throughput")
    ax.text(0, value, f"{value:.0f}", ha="center", va="bottom")
    fig.tight_layout()
    out = OUT_DIR / "phase7_write_throughput.png"
    fig.savefig(out, dpi=150)
    print(f"Wrote {out}")


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows = load_rows(CSV_PATH)
    plot_read_comparison(rows)
    plot_sstable_sweep(rows)
    plot_write_throughput(rows)


if __name__ == "__main__":
    main()
