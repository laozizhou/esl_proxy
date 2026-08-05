#!/bin/bash
# benchmark_dedup.sh - Compare baseline vs ENABLE_DAG_DEDUP scheduling throughput.
#
# Builds and runs the esl_proxy binary N times total for both variants,
# interleaved in rounds of ROUND_SIZE runs per variant (one build per variant
# per round, not one build per single run) rather than all-baseline-then-
# all-dedup. Straight block ordering was confirmed to let time-varying server
# drift land unevenly across the two variants and produce a misleading
# difference (seen first-hand comparing add_successors_calls block-ordered
# vs interleaved) -- rounds keep that drift split fairly between both
# variants while still keeping the rebuild count manageable (N/ROUND_SIZE
# rounds x 2 builds, instead of N x 2 builds for full per-run interleaving).
#
# Reports median/mean/min/max for every [init]/[orchestration]/[scheduler]/
# [cutter]/[total] timing and counter line each run prints, including the
# idle/active/drain phase split and loop/busy iteration counts for
# [scheduler]/[cutter], and the add_successors_* fields in [cutter].
#
# Usage (run from the esl_proxy/esl_proxy repo root):
#   ./src/benchmark_dedup.sh [N_RUNS] [CASE] [SPMD_TIER] [ROUND_SIZE]
#
# Defaults: N_RUNS=30, CASE=qwen3_dynamic_tensormap.h, SPMD_TIER=2,
# ROUND_SIZE=10.
# Note: QWEN3_SPMD_TIER=0 is a known-broken configuration unrelated to
# dedup (a pre-existing fixed-size successor-list overflow in cutter.c);
# avoid it here.

set -u

N=${1:-30}
CASE=${2:-qwen3_dynamic_tensormap.h}
TIER=${3:-2}
ROUND_SIZE=${4:-10}

# "field|grep -oP pattern" -- field names become the DATA[] keys below.
FIELD_SPECS=(
    'init_time|\[init\] elapsed_time = \K[0-9]+'
    'orch_time|\[orchestration\] elapsed_time = \K[0-9]+'
    'orch_tp|\[orchestration\] task_tp = \K[0-9.]+'
    'sched_dur|\[scheduler\] duration = \K[0-9]+'
    'sched_tp|\[scheduler\] task_tp = \K[0-9.]+'
    'sched_idle|\[scheduler\] idle_ns = \K[0-9]+'
    'sched_active|\[scheduler\] active_ns = \K[0-9]+'
    'sched_drain|\[scheduler\] drain_ns = \K[0-9]+'
    'sched_idle_iters|\[scheduler\] idle_loop_iters = \K[0-9]+'
    'sched_idle_busy|\[scheduler\] idle_busy_iters = \K[0-9]+'
    'sched_active_iters|\[scheduler\] active_loop_iters = \K[0-9]+'
    'sched_active_busy|\[scheduler\] active_busy_iters = \K[0-9]+'
    'sched_drain_iters|\[scheduler\] drain_loop_iters = \K[0-9]+'
    'sched_drain_busy|\[scheduler\] drain_busy_iters = \K[0-9]+'
    'cutter_dur|\[cutter\] duration = \K[0-9]+'
    'cutter_tp|\[cutter\] task_tp = \K[0-9.]+'
    'cutter_idle|\[cutter\] idle_ns = \K[0-9]+'
    'cutter_active|\[cutter\] active_ns = \K[0-9]+'
    'cutter_drain|\[cutter\] drain_ns = \K[0-9]+'
    'cutter_idle_iters|\[cutter\] idle_loop_iters = \K[0-9]+'
    'cutter_idle_busy|\[cutter\] idle_busy_iters = \K[0-9]+'
    'cutter_active_iters|\[cutter\] active_loop_iters = \K[0-9]+'
    'cutter_active_busy|\[cutter\] active_busy_iters = \K[0-9]+'
    'cutter_drain_iters|\[cutter\] drain_loop_iters = \K[0-9]+'
    'cutter_drain_busy|\[cutter\] drain_busy_iters = \K[0-9]+'
    'as_ns|\[cutter\] add_successors_ns = \K[0-9]+'
    'as_share|\[cutter\] add_successors_share = \K[0-9.]+'
    'as_calls|\[cutter\] add_successors_calls = \K[0-9]+'
    'as_nonzero|\[cutter\] add_successors_nonzero_calls = \K[0-9]+'
    'total_time|\[total\] elapsed_time = \K[0-9]+'
    'total_tp|\[total\] task_tp = \K[0-9.]+'
)

declare -A DATA

stats() {
    local name=$1
    shift
    local result
    result=$(printf '%s\n' "$@" | sort -n | awk '
        { a[NR] = $1; sum += $1 }
        END {
            n = NR
            mean = sum / n
            if (n % 2 == 1) median = a[(n + 1) / 2]
            else median = (a[n / 2] + a[n / 2 + 1]) / 2
            p90_idx = int(0.90 * n + 0.5); if (p90_idx < 1) p90_idx = 1; if (p90_idx > n) p90_idx = n
            p99_idx = int(0.99 * n + 0.5); if (p99_idx < 1) p99_idx = 1; if (p99_idx > n) p99_idx = n
            printf "median=%.1f mean=%.1f min=%.1f max=%.1f p90=%.1f p99=%.1f", median, mean, a[1], a[n], a[p90_idx], a[p99_idx]
        }')
    echo "$name: $result"
}

build() {
    local label=$1
    local extra_cflags=$2

    echo "Building ($label)..." >&2
    make clean >/dev/null 2>&1
    # Override LDFLAGS to drop the Makefile's default -static: this server has
    # no glibc-static/libatomic-static installed, and static linking isn't
    # needed here since we build and run on this same machine.
    if [ -n "$extra_cflags" ]; then
        make CASE="$CASE" QWEN3_SPMD_TIER="$TIER" EXTRA_CFLAGS="$extra_cflags" LDFLAGS="-lpthread -latomic" >/dev/null 2>&1
    else
        make CASE="$CASE" QWEN3_SPMD_TIER="$TIER" LDFLAGS="-lpthread -latomic" >/dev/null 2>&1
    fi
    if [ ! -x bin/esl_proxy ]; then
        echo "ERROR: build failed for $label, bin/esl_proxy not found" >&2
        exit 1
    fi
}

run_round() {
    local label=$1
    local extra_cflags=$2
    local count=$3

    build "$label" "$extra_cflags"

    for ((i = 0; i < count; i++)); do
        out=$(WORKER_LOG= ./bin/esl_proxy 2>&1)
        for spec in "${FIELD_SPECS[@]}"; do
            local field=${spec%%|*}
            local pattern=${spec#*|}
            local value
            value=$(echo "$out" | grep -oP "$pattern")
            DATA["${label}_${field}"]+="$value "
        done
    done
}

remaining=$N
while [ "$remaining" -gt 0 ]; do
    this_round=$ROUND_SIZE
    if [ "$remaining" -lt "$ROUND_SIZE" ]; then
        this_round=$remaining
    fi
    run_round "baseline" "" "$this_round"
    run_round "dedup" "-DENABLE_DAG_DEDUP" "$this_round"
    remaining=$((remaining - this_round))
done

report() {
    local label=$1
    echo "=== $label ($N runs, CASE=$CASE, SPMD_TIER=$TIER, ROUND_SIZE=$ROUND_SIZE) ==="
    for spec in "${FIELD_SPECS[@]}"; do
        local field=${spec%%|*}
        stats "$field" ${DATA["${label}_${field}"]}
    done
    echo ""
}

report "baseline"
report "dedup"
