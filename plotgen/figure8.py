import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker


# Per-configuration overhead on SPEC2017, every Alaska build config relative to the
# baseline (plain bundled clang). The configs mirror benchmarks/figure8.py.
CONFIG_ORDER = [
    "noservice",
    "anchorage",
    "refcount",
    "refcount-gc",
    "refcount-gc-anchorage",
]

config_colors = {
    "noservice": "#0075ab",
    "anchorage": "#33bb88",
    "refcount": "#aa6fc5",
    "refcount-gc": "#ff6583",
    "refcount-gc-anchorage": "#ffa600",
}

config_labels = {
    "noservice": "noservice",
    "anchorage": "anchorage",
    "refcount": "RC",
    "refcount-gc": "RC+GC",
    "refcount-gc-anchorage": "RC+GC+anchorage",
}


def short_name(benchmark):
    # '605.mcf_s' -> 'mcf'
    try:
        return benchmark.split('.')[1].split('_')[0]
    except (IndexError, AttributeError):
        return str(benchmark)


df = pd.read_csv('results/figure8.csv')
df = df[df['suite'] == 'SPEC2017']
df['key'] = df['suite'] + '@' + df['benchmark']

metric = 'time'
baselines = df[df['config'] == 'baseline']
others = df[df['config'] != 'baseline']

dfs = []
for key in baselines['key'].unique():
    mean = baselines[baselines['key'] == key][metric].mean()
    if mean == 0:
        continue
    filt = pd.DataFrame(others[others['key'] == key])
    filt['benchmark'] = short_name(key.split('@')[1])
    filt['overhead'] = (filt[metric] - mean) / mean
    dfs.append(filt)

df = pd.concat(dfs) if dfs else pd.DataFrame(columns=['benchmark', 'config', 'overhead'])
present = [c for c in CONFIG_ORDER if c in set(df['config'])]

f, ax = plt.subplots(1, figsize=(7, 2.0), dpi=300)
g = sns.barplot(data=df, x='benchmark', y='overhead', hue='config',
                hue_order=present, palette=config_colors,
                errorbar=None, linewidth=0.8, edgecolor='black', ax=ax)
g.set(xlabel=None, ylabel=None)
g.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f'{int(y * 100)}%'))

handles, _ = ax.get_legend_handles_labels()
ax.legend(handles, [config_labels.get(c, c) for c in present],
          ncol=2, loc="upper right", fontsize=7, frameon=False)
plt.xticks(rotation=0, fontsize=7)
plt.grid(axis='y', linestyle='-', alpha=0.3, zorder=1)
plt.gca().set_axisbelow(True)
plt.tight_layout()

plt.savefig('results/figure8.pdf')
print("Wrote results/figure8.pdf")
