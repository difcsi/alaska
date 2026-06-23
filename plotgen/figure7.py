import os
import re
import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as mtick


def geo_mean(iterable):
    a = np.array(list(iterable), dtype=float) + 1
    return (a.prod() ** (1.0 / len(a))) - 1


# The Alaska build configurations, in the order they should appear in the legend.
# "baseline" is the reference every config's overhead is measured against, so it is
# not itself drawn as a bar.
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


def load_overheads(csv_path, metric='time'):
    """Return per-(suite,benchmark,config) overhead relative to the baseline run."""
    df = pd.read_csv(csv_path)
    # Average repeated runs, then pivot so each config sits beside its baseline.
    means = df.groupby(['suite', 'benchmark', 'config'])[metric].mean().reset_index()
    piv = means.pivot_table(index=['suite', 'benchmark'],
                            columns='config', values=metric).reset_index()
    if 'baseline' not in piv.columns:
        raise SystemExit("figure7: no 'baseline' config in results; cannot compute overhead")

    rows = []
    for cfg in CONFIG_ORDER:
        if cfg not in piv.columns:
            print(f"figure7: config '{cfg}' missing from results, skipping")
            continue
        for _, r in piv.iterrows():
            bl, v = r.get('baseline'), r.get(cfg)
            if pd.isna(bl) or pd.isna(v) or bl == 0:
                continue
            rows.append({'suite': r['suite'], 'benchmark': r['benchmark'],
                         'config': cfg, 'overhead': (v - bl) / bl})
    return pd.DataFrame(rows)


def summarize_by_suite(ov):
    """Geometric-mean overhead per (suite, config), plus an 'ALL' rollup."""
    out = []
    for (suite, cfg), grp in ov.groupby(['suite', 'config']):
        out.append({'suite': suite, 'config': cfg, 'overhead': geo_mean(grp['overhead'])})
    for cfg, grp in ov.groupby('config'):
        out.append({'suite': 'ALL', 'config': cfg, 'overhead': geo_mean(grp['overhead'])})
    return pd.DataFrame(out)


def main():
    ov = load_overheads('bench/results/figure7/all.csv')
    present = [c for c in CONFIG_ORDER if c in set(ov['config'])]
    summary = summarize_by_suite(ov)

    suite_order = [s for s in ['Embench', 'GAP', 'NAS', 'SPEC2017', 'PolyBench'] if s in set(summary['suite'])]
    suite_order = suite_order + [s for s in summary['suite'].unique() if s not in suite_order and s != 'ALL'] + ['ALL']

    f, ax = plt.subplots(1, figsize=(9, 2.6), dpi=300)
    plt.grid(axis='y', linestyle='-', alpha=0.3, zorder=0)
    g = sns.barplot(data=summary, x='suite', y='overhead', hue='config',
                    order=suite_order, hue_order=present,
                    palette=config_colors, edgecolor='black', linewidth=0.8, ax=ax)

    ax.axhline(y=0, linewidth=1, color='black')
    ax.yaxis.set_major_formatter(mtick.FuncFormatter(lambda y, _: f'{int(y * 100)}%'))
    ax.set(xlabel=None, ylabel='Exec. time overhead (geomean)')

    handles, _ = ax.get_legend_handles_labels()
    ax.legend(handles, [config_labels.get(c, c) for c in present],
              ncol=len(present), loc='upper center', bbox_to_anchor=(0.5, 1.18),
              fontsize=7, frameon=False)
    ax.set_axisbelow(True)
    plt.tight_layout()

    os.makedirs('results/', exist_ok=True)
    plt.savefig('results/figure7.pdf', bbox_inches='tight', pad_inches=.05)
    print("Wrote results/figure7.pdf")


if __name__ == '__main__':
    main()
