"""Plot the runtime event tallies collected by the figure-7 sweep.

The measurement build of libalaska (built with ALASKA_MEASURE=1) records, per run,
how many refcount increments/decrements and GC frees happened and how many heap
compaction passes ran / objects they relocated. EventCountingRunner in
benchmarks/figure7.py folds those into bench/results/figure7/all.csv as extra
metric columns; this script summarizes them.

Outputs (under results/):
  figure7_events.pdf              -- per-config totals, one panel per event metric
  figure7_events_per_benchmark.pdf -- per-benchmark breakdown, one panel per metric
  figure7_events.csv              -- the per-config totals as a table

Run a measurement sweep first, otherwise every count is 0:
  ALASKA_MEASURE=1 ./build.sh && python benchmarks/figure7.py
"""

import os
import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as mtick


# Event metrics written by the runtime dump (EventCounters.hpp), with the labels
# used on each plot panel. Only those actually present in the results are drawn.
EVENT_METRICS = [
    ("halloc", "Handle allocations"),
    ("hfree", "Handle frees (program)"),
    ("incref", "Refcount increments"),
    ("decref", "Refcount decrements"),
    ("gc_frees", "GC frees"),
    ("compactions", "Compaction passes"),
    ("objects_moved", "Objects relocated"),
    ("handles_total", "Handles at teardown"),
    ("handles_nonzero_rc", "Handles w/ nonzero refcount at teardown"),
]

# Configs to draw, in legend order. "baseline" is plain clang with no Alaska
# runtime, so it produces no events and is omitted. Mirrors plotgen/figure7.py.
CONFIG_ORDER = [
    "noservice",
    "anchorage",
    "refcount",
    "refcount-anchorage",
    "refcount-gc",
    "refcount-gc-anchorage",
]

config_colors = {
    "noservice": "#0075ab",
    "anchorage": "#33bb88",
    "refcount": "#aa6fc5",
    "refcount-anchorage": "#8c564b",
    "refcount-gc": "#ff6583",
    "refcount-gc-anchorage": "#ffa600",
}

config_labels = {
    "noservice": "noservice",
    "anchorage": "anchorage",
    "refcount": "RC",
    "refcount-anchorage": "RC+anchorage",
    "refcount-gc": "RC+GC",
    "refcount-gc-anchorage": "RC+GC+anchorage",
}

SUITE_ORDER = ["Embench", "GAP", "NAS", "SPEC2017", "PolyBench"]


def ordered_suites(suites):
    present = set(suites)
    head = [s for s in SUITE_ORDER if s in present]
    tail = sorted(s for s in present if s not in SUITE_ORDER)
    return head + tail


def load_counts(csv_path):
    """Per-(suite,benchmark,config) mean event counts for the metrics present.

    Returns (long_df, metrics) where long_df has columns suite, benchmark, config
    and one column per available event metric (runs averaged)."""
    df = pd.read_csv(csv_path)
    metrics = [m for m, _ in EVENT_METRICS if m in df.columns]
    if not metrics:
        raise SystemExit(
            "figure7_events: results have no event-count columns. Rebuild with "
            "ALASKA_MEASURE=1 and re-run the figure-7 sweep.")
    means = (df.groupby(["suite", "benchmark", "config"])[metrics]
               .mean().reset_index())
    # Drop baseline (and anything not in CONFIG_ORDER) -- they carry no events.
    means = means[means["config"].isin(CONFIG_ORDER)]
    return means, metrics


def present_configs(means):
    return [c for c in CONFIG_ORDER if c in set(means["config"])]


def plot_totals(means, metrics, present, out_path):
    """One panel per metric: total event count per config, summed over benchmarks."""
    totals = means.groupby("config")[metrics].sum().reset_index()

    n = len(metrics)
    ncol = min(n, 3)
    nrow = (n + ncol - 1) // ncol
    f, axes = plt.subplots(nrow, ncol, figsize=(4.2 * ncol, 3.0 * nrow), dpi=300,
                           squeeze=False)
    label_by_metric = dict(EVENT_METRICS)

    for idx, metric in enumerate(metrics):
        ax = axes[idx // ncol][idx % ncol]
        sub = totals.set_index("config").reindex(present)[metric].fillna(0)
        colors = [config_colors[c] for c in present]
        ax.bar(range(len(present)), sub.values, color=colors,
               edgecolor="black", linewidth=0.6, zorder=3)
        ax.grid(axis="y", linestyle="-", alpha=0.3, zorder=0)
        ax.set_title(label_by_metric.get(metric, metric), fontsize=9)
        ax.set_xticks(range(len(present)))
        ax.set_xticklabels([config_labels.get(c, c) for c in present],
                           rotation=45, ha="right", fontsize=7)
        ax.yaxis.set_major_formatter(mtick.FuncFormatter(_si))
        ax.set_axisbelow(True)

    # Blank any unused panels in the grid.
    for idx in range(n, nrow * ncol):
        axes[idx // ncol][idx % ncol].axis("off")

    f.suptitle("Runtime event totals (summed over all benchmarks)", fontsize=10)
    plt.tight_layout(rect=(0, 0, 1, 0.97))
    plt.savefig(out_path, bbox_inches="tight", pad_inches=0.05)
    plt.close(f)
    print(f"Wrote {out_path}")


def plot_per_benchmark(means, metrics, present, out_path):
    """One panel per metric: grouped bars over benchmarks, hued by config."""
    # Stable benchmark ordering, grouped by suite.
    order = []
    for suite in ordered_suites(means["suite"].unique()):
        order += sorted(means[means["suite"] == suite]["benchmark"].unique())

    n = len(metrics)
    width = max(9, 0.45 * len(order))
    f, axes = plt.subplots(n, 1, figsize=(width, 2.6 * n), dpi=300, squeeze=False)
    label_by_metric = dict(EVENT_METRICS)

    for idx, metric in enumerate(metrics):
        ax = axes[idx][0]
        ax.grid(axis="y", linestyle="-", alpha=0.3, zorder=0)
        sns.barplot(data=means, x="benchmark", y=metric, hue="config",
                    order=order, hue_order=present, palette=config_colors,
                    edgecolor="black", linewidth=0.5, ax=ax)
        ax.set(xlabel=None, ylabel=label_by_metric.get(metric, metric))
        ax.yaxis.set_major_formatter(mtick.FuncFormatter(_si))
        ax.set_axisbelow(True)
        if ax.get_legend() is not None:
            ax.get_legend().remove()
        if idx == n - 1:
            ax.tick_params(axis="x", labelsize=6)
            plt.setp(ax.get_xticklabels(), rotation=60, ha="right",
                     rotation_mode="anchor")
        else:
            ax.set_xticklabels([])

    handles = [plt.Rectangle((0, 0), 1, 1, color=config_colors[c]) for c in present]
    f.legend(handles, [config_labels.get(c, c) for c in present],
             ncol=len(present), loc="upper center", fontsize=7, frameon=False,
             bbox_to_anchor=(0.5, 1.0))
    plt.tight_layout(rect=(0, 0, 1, 0.98))
    plt.savefig(out_path, bbox_inches="tight", pad_inches=0.05)
    plt.close(f)
    print(f"Wrote {out_path}")


def _si(value, _pos=None):
    """Compact axis tick labels: 1500000 -> '1.5M'."""
    for scale, suffix in ((1e9, "B"), (1e6, "M"), (1e3, "k")):
        if abs(value) >= scale:
            return f"{value / scale:.1f}{suffix}"
    return f"{int(value)}"


def main():
    means, metrics = load_counts("bench/results/figure7/all.csv")
    present = present_configs(means)

    os.makedirs("results/", exist_ok=True)

    totals = means.groupby("config")[metrics].sum().reindex(present)
    totals.to_csv("results/figure7_events.csv")
    print("Wrote results/figure7_events.csv")

    plot_totals(means, metrics, present, "results/figure7_events.pdf")
    plot_per_benchmark(means, metrics, present,
                       "results/figure7_events_per_benchmark.pdf")


if __name__ == "__main__":
    main()
