"""Render docs/*.png from the CSVs written by bench_lob.

    python bench/plot.py [results_dir] [out_dir]
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RES = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/results")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "docs")
OUT.mkdir(parents=True, exist_ok=True)

# Okabe-Ito blue / orange: colour-blind safe pair, fixed per implementation.
COLORS = {"lob::OrderBook": "#0072B2", "std baseline": "#E69F00"}
INK, MUTED, GRID = "#1f2328", "#59636e", "#d8dee4"

plt.rcParams.update({
    "font.size": 10, "axes.edgecolor": GRID, "axes.labelcolor": INK,
    "xtick.color": MUTED, "ytick.color": MUTED, "text.color": INK,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.axisbelow": True, "figure.dpi": 150, "savefig.bbox": "tight",
})


def rows(name):
    with open(RES / name, newline="") as f:
        return list(csv.DictReader(f))


env = {r["key"]: r["value"] for r in rows("environment.csv")}
footer = f'{env["cpu"].strip()} | GCC {env["compiler"]} | {env["flags"]} | seed {env["seed"]}'


# ---- 1. throughput bars ------------------------------------------------------
tp = defaultdict(list)
for r in rows("throughput.csv"):
    tp[r["impl"]].append(float(r["m_events_per_sec"]))
fig, ax = plt.subplots(figsize=(5.2, 3.8))
names = ["std baseline", "lob::OrderBook"]
for i, n in enumerate(names):
    v = tp[n]
    med = statistics.median(v)
    ax.bar(i, med, width=0.55, color=COLORS[n], zorder=3)
    ax.errorbar(i, med, yerr=[[med - min(v)], [max(v) - med]], color=INK, capsize=4, lw=1, zorder=4)
    ax.text(i + 0.32, med, f"{med:.1f}", va="center", color=INK)
ax.set_xticks(range(len(names)), names)
ax.set_ylabel("Throughput (M events/s)")
ax.set_title(f'Replay of {int(env["events"]) / 1e6:.0f}M events (60% add / 35% cancel / 5% marketable)', fontsize=10)
ax.grid(axis="x", visible=False)
fig.text(0.01, -0.12, f"median of {len(tp[names[0]])} runs, whiskers = min/max\n{footer}", fontsize=7, color=MUTED)
fig.savefig(OUT / "throughput.png")
plt.close(fig)

# ---- 2. latency CDFs ---------------------------------------------------------
cdf = defaultdict(lambda: ([], []))
for r in rows("latency_cdf.csv"):
    k = (r["op"], r["impl"])
    cdf[k][0].append(float(r["ns"]))
    cdf[k][1].append(float(r["quantile"]))
gran = float(env.get("timer_granularity_ns", 10))
fig, axes = plt.subplots(1, 3, figsize=(11, 3.6), sharey=True)
for ax, op in zip(axes, ["add", "match", "cancel"]):
    for n in names[::-1]:
        x, q = cdf[(op, n)]
        x = [max(v, gran / 2) for v in x]  # log axis cannot show 0 ns
        ax.step(x, q, where="post", color=COLORS[n], lw=1.6, label=n)
    ax.set_xscale("log")
    ax.set_title(op, fontsize=10)
    ax.set_xlabel("latency (ns, log scale)")
    ax.set_ylim(0, 1.0)
    for qv in (0.5, 0.99, 0.999):
        ax.axhline(qv, color=GRID, lw=0.8, ls=":")
axes[0].set_ylabel("cumulative fraction")
axes[0].legend(frameon=False, loc="lower right")
fig.suptitle("Per-operation latency CDF", y=1.02, fontsize=11)
fig.text(0.01, -0.16, f"rdtsc per event; timer granularity ~{gran:.0f} ns, back-to-back overhead ~{env['timer_overhead_ns']} ns (not subtracted)\n{footer}",
         fontsize=7, color=MUTED)
fig.savefig(OUT / "latency_cdf.png")
plt.close(fig)

# ---- 3. depth sweep ----------------------------------------------------------
ds = defaultdict(lambda: ([], []))
for r in rows("depth_sweep.csv"):
    ds[r["impl"]][0].append(int(r["depth"]))
    ds[r["impl"]][1].append(float(r["m_events_per_sec"]))
fig, ax = plt.subplots(figsize=(5.6, 3.8))
for n in names[::-1]:
    x, y = ds[n]
    ax.plot(x, y, marker="o", ms=6, lw=2, color=COLORS[n], label=n)
    ax.annotate(f"{y[-1]:.1f}", (x[-1], y[-1]), textcoords="offset points", xytext=(6, 0), va="center", fontsize=8, color=INK)
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("resting orders in book (log scale)")
ax.set_ylabel("Throughput (M events/s, log scale)")
ax.set_title("Throughput vs book depth", fontsize=10)
ax.legend(frameon=False)
fig.text(0.01, -0.12, f"depth-regulated stream, ~5% marketable; book capacity sized to depth\n{footer}", fontsize=7, color=MUTED)
fig.savefig(OUT / "depth_sweep.png")
plt.close(fig)
print("wrote", *(OUT / n for n in ("throughput.png", "latency_cdf.png", "depth_sweep.png")))
