#!/usr/bin/env bash
# keep_raw_events.sh <alaska-bin> <source.c> <workdir>
#
# Keep-raw (leaving provably non-escaping malloc/calloc on the libc allocator) is
# ON by default in the compiler; --no-keep-raw disables it. This builds <source.c>
# both ways from the one toolchain and checks:
#   * both builds produce identical program output and exit 0 (transparency), and
#   * (if the runtime has event counters compiled in) the default build performs
#     strictly fewer hallocs than the --no-keep-raw baseline.
#
# The halloc check SKIPS (does not fail) when counters are not compiled in
# (-DALASKA_ENABLE_EVENT_COUNTERS=ON / ALASKA_MEASURE=1).
set -euo pipefail

# Make the alaska toolchain (gclang / get-bc / bundled llvm) available if the
# caller has not already sourced it. enable-toolchain lives one dir up from test/.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
if ! command -v gclang >/dev/null 2>&1 && [ -f "$HERE/../opt/enable-toolchain" ]; then
  . "$HERE/../opt/enable-toolchain"
fi
export LDFLAGS="-fuse-ld=bfd ${LDFLAGS:-}"

ALASKA="$1"
SRC="$2"
WORK="$3"

OFF_BIN="$WORK/keep_raw_off"   # baseline: --no-keep-raw (every alloc -> handle)
ON_BIN="$WORK/keep_raw_on"     # default:  keep-raw on
EV_OFF="$WORK/keep_raw_ev_off.txt"
EV_ON="$WORK/keep_raw_ev_on.txt"

# Build both. Surface the keep-raw compile diagnostic ("[keep-raw] kept N ...")
# from the default build.
"$ALASKA" -O3 --no-keep-raw "$SRC" -o "$OFF_BIN" >/dev/null 2>&1
KEPT="$("$ALASKA" -O3 "$SRC" -o "$ON_BIN" 2>&1 | grep -E '\[keep-raw\] kept' || true)"
[ -n "$KEPT" ] && echo "$KEPT"

# Run both, capturing output + exit code (don't let a self-check FAIL abort us).
rm -f "$EV_OFF" "$EV_ON"
set +e
OUT_OFF="$(ALASKA_EVENT_LOG="$EV_OFF" "$OFF_BIN")"; RC_OFF=$?
OUT_ON="$(ALASKA_EVENT_LOG="$EV_ON" "$ON_BIN")"; RC_ON=$?
set -e

echo "[keep-raw] off: ${OUT_OFF//$'\n'/ | }"
echo "[keep-raw] on : ${OUT_ON//$'\n'/ | }"

if [ "$RC_OFF" -ne 0 ] || [ "$RC_ON" -ne 0 ]; then
  echo "[keep-raw] FAIL: a build exited nonzero (off=$RC_OFF on=$RC_ON)"
  exit 1
fi
if [ "$OUT_OFF" != "$OUT_ON" ]; then
  echo "[keep-raw] FAIL: program output differs between builds"
  exit 1
fi
echo "[keep-raw] PASS: transparent (identical output, both exit 0)"

# Halloc-delta check (needs event counters compiled in).
if [ ! -f "$EV_OFF" ] || [ ! -f "$EV_ON" ]; then
  echo "[keep-raw-events] event counters not compiled in (build with ALASKA_MEASURE=1 /"
  echo "                  -DALASKA_ENABLE_EVENT_COUNTERS=ON); skipping halloc-delta check."
  exit 0
fi

H_OFF="$(sed -n 's/^halloc=//p' "$EV_OFF")"
H_ON="$(sed -n 's/^halloc=//p' "$EV_ON")"
echo "[keep-raw-events] halloc off=$H_OFF on=$H_ON"

if [ -z "$H_OFF" ] || [ -z "$H_ON" ]; then
  echo "[keep-raw-events] FAIL: no halloc= line in event log"
  exit 1
fi
if [ "$H_ON" -lt "$H_OFF" ]; then
  echo "[keep-raw-events] PASS: kept $((H_OFF - H_ON)) allocation(s) off the handle path"
  exit 0
fi
echo "[keep-raw-events] FAIL: expected fewer hallocs with keep-raw on"
exit 1
