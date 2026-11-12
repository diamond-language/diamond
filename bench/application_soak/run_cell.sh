#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
workspace=$(realpath "${1:?workspace required}")
app=${2:?application required}; variant=${3:?variant required}; workers=${4:?worker count required}
case $app in pheint|SkindicateV3) ;; *) exit 64 ;; esac
case $variant in rc3|candidate) ;; *) exit 64 ;; esac
case $workers in 1|4) ;; *) exit 64 ;; esac
export SOAK_RSS_CAP_KIB=${SOAK_RSS_CAP_KIB:-2000000}
available=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
(( available > SOAK_RSS_CAP_KIB + 2000000 )) || { echo 'Insufficient free-memory margin for soak'; exit 1; }
export SOAK_RESULT_DIR=$workspace/results/${SOAK_LABEL_PREFIX:-}$app-$variant-w$workers
[[ ! -e $SOAK_RESULT_DIR ]] || { echo "Result directory already exists: $SOAK_RESULT_DIR"; exit 1; }
mkdir -p "$SOAK_RESULT_DIR"
export SOAK_PHASE_FILE=$SOAK_RESULT_DIR/phase
export SOAK_BINARY=$workspace/artifacts/$app-$variant
export SOAK_SERVER=$here/server.sh SOAK_WORKERS=$workers
export SOAK_WARMUP_SECONDS=${SOAK_WARMUP_SECONDS:-30} SOAK_QUIET_SECONDS=${SOAK_QUIET_SECONDS:-10}
export SOAK_FINAL_QUIET_SECONDS=${SOAK_FINAL_QUIET_SECONDS:-30}
export SOAK_ROUNDS=${SOAK_ROUNDS:-3} SOAK_SECONDS=${SOAK_SECONDS:-60}
export SOAK_PORT=18480 SOAK_SERVER_CPUS=0,2,4,6
export DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=32 LOG_LEVEL=off
driver=$workspace/candidate/diamond/build/d6
export DIAMOND_BIN=$driver
printf 'startup\n' > "$SOAK_PHASE_FILE"
printf '%s\n' "app=$app variant=$variant workers=$workers jit=1 threshold=32" > "$SOAK_RESULT_DIR/config.txt"
cat "$workspace/revisions.txt" >> "$SOAK_RESULT_DIR/config.txt"
child=
cleanup() {
    if [[ -n $child ]]; then
        kill -TERM -- "-$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM
cd "$workspace/apps/$app"
if [[ $app == pheint ]]; then
    setsid timeout --foreground --signal=TERM --kill-after=20 900 taskset -c 8-11 \
        "$driver" bench/bench.di --server "$SOAK_SERVER" --workers "$workers" --concurrency 8 \
        --players 64 --filler 1000 --only mixed --api-only --port "$SOAK_PORT" \
        --rounds "$SOAK_ROUNDS" --seconds "$SOAK_SECONDS" --idle-seconds 1 \
        --data-dir "$SOAK_RESULT_DIR" --json "$SOAK_RESULT_DIR/workload.json" \
        > "$SOAK_RESULT_DIR/workload.log" 2>&1 &
else
    setsid timeout --foreground --signal=TERM --kill-after=20 900 taskset -c 8-11 \
        "$driver" bench/soak.d6 > "$SOAK_RESULT_DIR/workload.log" 2>&1 &
fi
child=$!
status=0
wait "$child" || status=$?
cleanup
child=
trap - EXIT INT TERM
if [[ $status != 0 ]]; then tail -35 "$SOAK_RESULT_DIR/workload.log"; exit "$status"; fi
[[ -f $SOAK_RESULT_DIR/workload.json && -f $SOAK_RESULT_DIR/db-final.json ]]
if compgen -G "$SOAK_RESULT_DIR/*.cap-tripped" >/dev/null; then echo 'Memory watchdog tripped'; exit 1; fi
echo "$app $variant workers=$workers completed"
