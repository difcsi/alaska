"""
figure_compile.py — Compile-time overhead of each Alaska configuration.

Reads bench/results/figure7/compile_times.csv (written by benchmarks/figure7.py,
which wraps every pipeline stage in a TimedStage) and draws a grouped bar chart of
per-benchmark compile-time overhead relative to the baseline pipeline, one bar per
Alaska build configuration -- the same visual language as plotgen/figure7.py's
execution-time chart, with per-suite and overall geometric-mean rollups.

Outputs:
  results/figure_compile.pdf  -- the grouped overhead bar chart
  results/figure_compile.csv  -- the per-(suite,benchmark,config) overheads
"""

import os
import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as mtick


def geo_mean(iterable):
    a = np.array(list(iterable), dtype=float) + 1
    return (a.prod() ** (1.0 / len(a))) - 1


# Map ALASKA_SUITES keys (as used by benchmarks/figure7.py) to the suite names that
# appear in the results' `suite` column.
_SUITE_NAME = {
    "embench": "Embench", "gap": "GAP", "nas": "NAS", "olden": "Olden",
    "gcbench": "GCBench", "mibench": "MiBench", "spec2017": "SPEC2017",
    "polybench": "PolyBench",
}


def filter_runs(df):
    """Restrict a results frame to the suites/benchmarks selected by the same
    ALASKA_SUITES / ALASKA_BENCH / ALASKA_BENCH_QUICK env vars that
    benchmarks/figure7.py honors, so plotting a partial sweep shows only what was
    run rather than stale rows left in compile_times.csv by an earlier sweep.
    With none of the vars set, the frame is returned unchanged."""
    suites = [s.strip() for s in os.environ.get("ALASKA_SUITES", "").lower().split(",") if s.strip()]
    if suites:
        names = {_SUITE_NAME.get(s, s) for s in suites}
        df = df[df["suite"].isin(names)]
    benches = [b.strip() for b in os.environ.get("ALASKA_BENCH", "").split(",") if b.strip()]
    if benches:
        df = df[df["benchmark"].isin(benches)]
    # Quick mode skips the heavy NAS pseudo-apps (BT/SP/LU), mirroring figure7.py's
    # NAS_EXCLUDE, in case those rows are still present in the results.
    quick = os.environ.get("ALASKA_BENCH_QUICK", "").lower() in ("1", "true", "yes", "on")
    if quick:
        nas_excl = df["benchmark"].str.lower().str.startswith(("bt", "sp", "lu"))
        df = df[~((df["suite"] == "NAS") & nas_excl)]
    return df


# Mirrors plotgen/figure7.py: the Alaska build configurations in legend order.
# "baseline" is the reference every config's overhead is measured against, so it
# is not itself drawn as a bar.
CONFIG_ORDER = [
    "noservice",
    "anchorage",
    "refcount",
    "refcount-anchorage",
    "refcount-gc",
    "refcount-gc-anchorage",
    "refcount-gc-anchorage-defer",
]

config_colors = {
    "noservice": "#0075ab",
    "anchorage": "#33bb88",
    "refcount": "#aa6fc5",
    "refcount-anchorage": "#8c564b",
    "refcount-gc": "#ff6583",
    "refcount-gc-anchorage": "#ffa600",
    "refcount-gc-anchorage-defer": "#003f5c",
}

config_labels = {
    "noservice": "noservice",
    "anchorage": "anchorage",
    "refcount": "RC",
    "refcount-anchorage": "RC+anchorage",
    "refcount-gc": "RC+GC",
    "refcount-gc-anchorage": "RC+GC+anchorage",
    "refcount-gc-anchorage-defer": "RC+GC+anchorage+defer",
}

SUITE_ORDER = ['Embench', 'GAP', 'NAS', 'SPEC2017', 'PolyBench']


def ordered_suites(suites):
    """Known suites first (canonical order), then any extras alphabetically."""
    present = set(suites)
    head = [s for s in SUITE_ORDER if s in present]
    tail = sorted(s for s in present if s not in SUITE_ORDER)
    return head + tail


def load_overheads(csv_path):
    """Return per-(suite,benchmark,config) compile-time overhead vs baseline.

    Sums each pipeline's stage times into one total compile time per benchmark
    (averaging any repeated compiles of the same stage first), then expresses each
    Alaska config's total as a fractional overhead over the baseline pipeline."""
    df = pd.read_csv(csv_path)
    df = filter_runs(df)

    # Average repeated compiles of the same stage, then sum stages -> total time
    # per (suite, benchmark, pipeline).
    per_stage = (df.groupby(['suite', 'benchmark', 'pipeline', 'stage'])['compile_time']
                   .mean().reset_index())
    totals = (per_stage.groupby(['suite', 'benchmark', 'pipeline'])['compile_time']
                       .sum().reset_index())

    piv = totals.pivot_table(index=['suite', 'benchmark'],
                             columns='pipeline', values='compile_time').reset_index()
    if 'baseline' not in piv.columns:
        raise SystemExit("figure_compile: no 'baseline' pipeline in compile_times.csv; "
                         "cannot compute overhead")

    rows = []
    for cfg in CONFIG_ORDER:
        if cfg not in piv.columns:
            print(f"figure_compile: config '{cfg}' missing from results, skipping")
            continue
        for _, r in piv.iterrows():
            bl, v = r.get('baseline'), r.get(cfg)
            if pd.isna(bl) or pd.isna(v) or bl == 0:
                continue
            rows.append({'suite': r['suite'], 'benchmark': r['benchmark'],
                         'config': cfg, 'overhead': (v - bl) / bl})
    return pd.DataFrame(rows)


def build_plot_frame(ov):
    """Long frame with one row per (benchmark, config) overhead, plus per-suite
    and overall geometric-mean rollups. Returns the frame and the ordered x labels."""
    rows = []
    order = []

    for suite in ordered_suites(ov['suite'].unique()):
        sub = ov[ov['suite'] == suite]
        for bench in sorted(sub['benchmark'].unique()):
            order.append(bench)
            for _, r in sub[sub['benchmark'] == bench].iterrows():
                rows.append({'label': bench, 'suite': suite, 'config': r['config'],
                             'overhead': r['overhead'], 'is_geomean': False})
        glabel = f'{suite} geomean'
        order.append(glabel)
        for cfg, grp in sub.groupby('config'):
            rows.append({'label': glabel, 'suite': suite, 'config': cfg,
                         'overhead': geo_mean(grp['overhead']), 'is_geomean': True})

    order.append('geomean')
    for cfg, grp in ov.groupby('config'):
        rows.append({'label': 'geomean', 'suite': 'ALL', 'config': cfg,
                     'overhead': geo_mean(grp['overhead']), 'is_geomean': True})

    return pd.DataFrame(rows), order


def main():
    ov = load_overheads('bench/results/figure7/compile_times.csv')
    if ov.empty:
        raise SystemExit("figure_compile: no overlapping baseline/config compile times")

    os.makedirs('results/', exist_ok=True)
    ov.to_csv('results/figure_compile.csv', index=False)
    print("Wrote results/figure_compile.csv")

    present = [c for c in CONFIG_ORDER if c in set(ov['config'])]
    plot_df, order = build_plot_frame(ov)

    width = max(9, 0.55 * len(order))
    f, ax = plt.subplots(1, figsize=(width, 3.2), dpi=300)
    plt.grid(axis='y', linestyle='-', alpha=0.3, zorder=0)
    g = sns.barplot(data=plot_df, x='label', y='overhead', hue='config',
                    order=order, hue_order=present,
                    palette=config_colors, edgecolor='black', linewidth=0.6, ax=ax)

    # Hatch the geomean bars so they read as summaries, not individual tests.
    geomean_x = {order.index(lbl) for lbl in order if lbl.endswith('geomean')}
    for i, patch in enumerate(ax.patches):
        if (i % len(order)) in geomean_x:
            patch.set_hatch('//')

    ax.axhline(y=0, linewidth=1, color='black')
    ax.yaxis.set_major_formatter(mtick.FuncFormatter(lambda y, _: f'{int(y * 100)}%'))
    ax.set(xlabel=None, ylabel='Compile-time overhead')
    ax.tick_params(axis='x', labelsize=6)
    plt.setp(ax.get_xticklabels(), rotation=60, ha='right', rotation_mode='anchor')

    handles, _ = ax.get_legend_handles_labels()
    ax.legend(handles, [config_labels.get(c, c) for c in present],
              ncol=len(present), loc='upper center', bbox_to_anchor=(0.5, 1.18),
              fontsize=7, frameon=False)
    ax.set_axisbelow(True)
    plt.tight_layout()

    plt.savefig('results/figure_compile.pdf', bbox_inches='tight', pad_inches=.05)
    print("Wrote results/figure_compile.pdf")


if __name__ == '__main__':
    main()
