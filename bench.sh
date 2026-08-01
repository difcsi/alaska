#!/usr/bin/env bash
# Generates figures 7-12 (PDFs) from scratch.
# Run from the project root after build.sh has completed all six Alaska configs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() { printf "\e[32m[bench]\e[0m %s\n" "$*"; }

err() { printf "\e[31m[bench ERROR]\e[0m %s\n" "$*" >&2; }

# Run a named step, logging stdout+stderr to results/<name>.log.
# On failure: print the tail of the log and abort.
step() {
  local name="$1"; shift
  local log="$ROOT/results/${name}.log"
  log "Running: ${name}"
  mkdir -p "$ROOT/results"
  if "$@" >"$log" 2>&1; then
    log "  done -> results/${name}.log"
  else
    local rc=$?
    err "Step '${name}' failed (exit ${rc}). Last 40 lines of log:"
    tail -40 "$log" >&2
    exit "$rc"
  fi
}

# ---------------------------------------------------------------------------
# Optional-feature detection
# ---------------------------------------------------------------------------

spec_location=""
for loc in ./ ~ /; do
  if [[ -f "${loc}/SPEC2017.tar.gz" ]]; then
    spec_location="$(realpath "${loc}/SPEC2017.tar.gz")"
    break
  fi
done

if [[ -z "$spec_location" ]]; then
  printf "\e[33m[bench] SPEC2017 not found — figure 7 will omit SPEC, figure 8 will be skipped.\e[0m\n"
else
  log "Found SPEC: $spec_location"
fi

generate_figure11=false
memory_kb=$(grep MemAvailable /proc/meminfo 2>/dev/null | awk '{print $2}' || echo 0)
memory_gb=$(( memory_kb / 1024 / 1024 ))
if (( memory_gb >= 180 )); then
  read -r -p "[bench] Enough memory detected (~${memory_gb} GiB). Generate figure 11? (y/N) " answer
  if [[ "$answer" =~ ^[Yy]$ ]]; then
    generate_figure11=true
  fi
else
  log "Skipping figure 11 (requires ~200 GiB; only ${memory_gb} GiB available)."
fi

# ---------------------------------------------------------------------------
# Python venv
# ---------------------------------------------------------------------------

log "Setting up Python virtual environment"
if [[ ! -d "$ROOT/venv" ]]; then
  virtualenv "$ROOT/venv"
fi
# shellcheck source=/dev/null
source "$ROOT/venv/bin/activate"
pip install -Ur "$ROOT/requirements.txt" -q
touch "$ROOT/venv/touchfile"

mkdir -p "$ROOT/results"

# ---------------------------------------------------------------------------
# Figure 7 — multi-suite performance overview (Embench, GAP, NAS, SPEC2017)
# ---------------------------------------------------------------------------

step "figure7-bench" bash -lc "
  . '$ROOT/venv/bin/activate'
  . '$ROOT/opt/enable-toolchain'
  ulimit -s unlimited
  cd '$ROOT'
  python3 -m benchmarks.figure7
"
cp "$ROOT/bench/results/figure7/all.csv" "$ROOT/results/figure7.csv"

step "figure7-plot" bash -lc "
  . '$ROOT/venv/bin/activate'
  cd '$ROOT'
  python3 '$ROOT/plotgen/figure7.py'
"

# ---------------------------------------------------------------------------
# Figure 8 — SPEC2017 per-config overhead (only if SPEC is available)
# ---------------------------------------------------------------------------

if [[ -n "$spec_location" ]]; then
  step "figure8-bench" bash -lc "
    . '$ROOT/venv/bin/activate'
    . '$ROOT/opt/enable-toolchain'
    ulimit -s unlimited
    cd '$ROOT'
    python3 -m benchmarks.figure8
  "
  cp "$ROOT/bench/results/figure8/all.csv" "$ROOT/results/figure8.csv"

  step "figure8-plot" bash -lc "
    . '$ROOT/venv/bin/activate'
    cd '$ROOT'
    python3 '$ROOT/plotgen/figure8.py'
  "
else
  log "Skipping figure 8 (no SPEC)."
fi

# ---------------------------------------------------------------------------
# rsstracker.so — memory-tracking helper used by Redis/Memcached figures
# ---------------------------------------------------------------------------

RSSTRACKER_SO="$ROOT/opt/alaska-refcount-gc-anchorage/lib/librsstracker.so"
if [[ ! -f "$RSSTRACKER_SO" ]]; then
  log "Building librsstracker.so"
  cc -shared -fPIC -pthread \
     -o "$RSSTRACKER_SO" \
     "$ROOT/runtime/extra/rsstracker.c" \
     -lpthread
fi

# ---------------------------------------------------------------------------
# Figure 9 — Redis fragmentation / Anchorage defragmentation
# ---------------------------------------------------------------------------

step "figure9-redis-build" bash -lc "
  . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
  make -C '$ROOT/redis' redis
"

step "figure9-bench+plot" bash -lc "
  . '$ROOT/venv/bin/activate'
  . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
  ulimit -s unlimited
  cd '$ROOT'
  python3 -m redis.frag
  python3 '$ROOT/plotgen/figure9.py'
"

# ---------------------------------------------------------------------------
# Figure 10 — Redis barrier pause-interval sweep
# ---------------------------------------------------------------------------

step "figure10-bench+plot" bash -lc "
  . '$ROOT/venv/bin/activate'
  . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
  ulimit -s unlimited
  cd '$ROOT'
  python3 -m redis.config_sweep
  python3 '$ROOT/plotgen/figure10.py'
"

# ---------------------------------------------------------------------------
# Figure 11 — Large-scale Redis fragmentation (~200 GiB required)
# ---------------------------------------------------------------------------

if [[ "$generate_figure11" == "true" ]]; then
  step "figure11-redis-build" bash -lc "
    . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
    make -C '$ROOT/redis' redis
  "

  step "figure11-bench" bash -lc "
    . '$ROOT/venv/bin/activate'
    . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
    ulimit -s unlimited
    cd '$ROOT'
    python3 -m redis.frag_large
  "

  step "figure11-plot" bash -lc "
    . '$ROOT/venv/bin/activate'
    cd '$ROOT'
    python3 '$ROOT/plotgen/figure11.py'
  "
else
  log "Skipping figure 11."
fi

# ---------------------------------------------------------------------------
# Figure 12 — Memcached YCSB latency analysis
# ---------------------------------------------------------------------------

step "figure12-memcached-build" bash -lc "
  . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
  make -C '$ROOT/memcached' memcached
"

step "figure12-bench" bash -lc "
  . '$ROOT/venv/bin/activate'
  . '$ROOT/opt/enable-alaska-refcount-gc-anchorage'
  ulimit -s unlimited
  cd '$ROOT'
  python3 -m memcached.ycsb
"

step "figure12-plot" bash -lc "
  . '$ROOT/venv/bin/activate'
  cd '$ROOT'
  python3 '$ROOT/plotgen/figure12.py'
"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

log "All done. PDFs written to results/:"
ls -1 "$ROOT/results/"*.pdf 2>/dev/null | sed 's|.*/|  |' || true
