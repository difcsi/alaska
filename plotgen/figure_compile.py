"""
figure_compile.py — Compile-time overhead of Alaska vs baseline.

Reads bench/results/figure7/compile_times.csv produced by benchmarks/figure7.py
and generates a bar chart of per-benchmark compile-time overhead (alaska / baseline - 1).
"""

import os
import re
import math
import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib as mpl
import matplotlib.pyplot as plt
import matplotlib.ticker as mtick
from collections import defaultdict


# ---------------------------------------------------------------------------
# Shared style helpers (mirrors plotgen/figure7.py)
# ---------------------------------------------------------------------------

plt.rc('xtick', labelsize=8)
plt.rc('ytick', labelsize=8)

colors = {
    "Embench":  "#0075ab",
    "GAP":      "#aa6fc5",
    "NAS":      "#ff6583",
    "SPEC2017": "#ffa600",
    "PolyBench":"#33bb88",
    "ALL":      "#54EE2E",
}

rename_table = {
    'sglib-combined': 'sglib',
}


def format_xtick(tick):
    tick = re.sub(r'^\d+\.', '', tick)
    tick = re.sub(r'_s$', '', tick)
    return rename_table.get(tick, tick)


def geo_mean(iterable):
    a = np.array(iterable) + 1
    return (a.prod() ** (1.0 / len(a))) - 1


# ---------------------------------------------------------------------------
# Aggregate compile_times.csv into per-benchmark total compile time
# ---------------------------------------------------------------------------

def load_compile_overhead(csv_path):
    """
    Read compile_times.csv and return a DataFrame with columns:
      suite, benchmark, overhead
    where overhead = (alaska_total - baseline_total) / baseline_total.
    """
    df = pd.read_csv(csv_path)

    # Sum all stage compile times per pipeline × benchmark.
    totals = (
        df.groupby(['suite', 'benchmark', 'pipeline'])['compile_time']
        .sum()
        .reset_index()
    )

    alaska   = totals[totals['pipeline'] == 'alaska'  ].rename(columns={'compile_time': 'alaska'})
    baseline = totals[totals['pipeline'] == 'baseline'].rename(columns={'compile_time': 'baseline'})

    merged = alaska.merge(baseline, on=['suite', 'benchmark'])
    merged['overhead'] = (merged['alaska'] - merged['baseline']) / merged['baseline']
    return merged[['suite', 'benchmark', 'overhead']]


# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------

def plot_compile_overhead(df, output, figsize=(9, 1.75), angle_dude=45,
                          geomean_parts=False, geomean_all=True):
    ylabel = 'overhead'
    bars = {'benchmark': [], 'suite': [], ylabel: []}
    suites = defaultdict(lambda: {'benchmark': [], ylabel: []})

    def add_bar(bench, suite, value):
        bars['benchmark'].append(suite + '@' + bench)
        bars['suite'].append(suite)
        bars[ylabel].append(value)
        suites[suite]['benchmark'].append(suite + '@' + bench)
        suites[suite][ylabel].append(value)

    for name, group in df.groupby('suite'):
        for _, row in group.iterrows():
            add_bar(row['benchmark'], row['suite'], row['overhead'])
        if geomean_parts:
            add_bar('geomean', name, geo_mean(group['overhead']))

    if geomean_all:
        add_bar('geomean', 'ALL', geo_mean(df['overhead']))

    plot_df = pd.DataFrame(bars)

    f, ax = plt.subplots(1, figsize=figsize)
    plt.grid(axis='y')

    g = sns.barplot(
        data=plot_df,
        x='benchmark',
        y=ylabel,
        hue='suite',
        linewidth=1,
        edgecolor='black',
        palette=colors,
        ax=ax,
        dodge=False,
    )

    max_val = max(plot_df[ylabel])
    min_val = min(plot_df[ylabel])
    top    = max_val * 1.2
    bottom = min(0, min_val * 1.2)
    g.set_ylim((bottom, top))

    g.set_xticklabels(
        map(lambda x: format_xtick(x.split('@')[1]),
            (item.get_text() for item in g.get_xticklabels()))
    )

    plt.legend([], [], frameon=False)
    plt.axhline(y=0, linewidth=1, color='black')

    for _, hue in enumerate(plot_df['suite'].unique()[:-1]):
        xpos = plot_df.loc[plot_df['suite'] == hue].index[-1] + 0.5
        plt.axvline(x=xpos, linewidth=1, color='black')

    for name, group in plot_df.groupby('suite'):
        xpos = max(0, np.mean(group['benchmark'].index))
        plt.text(xpos, top, name, ha='center', va='bottom', fontsize=10)

    for x, row in plot_df.iterrows():
        y = row[ylabel]
        perc = y * 100
        g.annotate(
            f'{perc:.0f}',
            (x, max(y, 0)),
            ha='center', va='bottom',
            xytext=(0, 1), fontsize=6.5,
            rotation=0, textcoords='offset points',
            zorder=11,
        )

    g.set(title=None, xlabel=None, ylabel=None)
    g.yaxis.set_major_formatter(
        mtick.FuncFormatter(lambda y, _: '{}%'.format(int(y * 100)))
    )
    plt.xticks(rotation=angle_dude, fontsize=7, rotation_mode='anchor', ha='right')
    plt.grid(visible=True, which='minor', linestyle='-', alpha=0.2, zorder=1)
    ax.set_axisbelow(True)
    plt.tight_layout()

    os.makedirs('results/', exist_ok=True)
    plt.savefig('results/' + output, bbox_inches='tight', pad_inches=0.05)
    print(f'Saved {output}')


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

plot_compile_overhead(
    load_compile_overhead('bench/results/figure7/compile_times.csv'),
    output='figure_compile.pdf',
    figsize=(9, 1.75),
    angle_dude=45,
    geomean_parts=False,
)
