#!/bin/bash
# benchmark_dedup.sh - Compare baseline vs ENABLE_DAG_DEDUP scheduling throughput.
#
# Builds and runs the esl_proxy binary N times for both variants (with make
# clean between every build, so EXTRA_CFLAGS toggles are never silently
# skipped), and reports median/mean/min/max for the [init]/[orchestration]/
# [scheduler]/[cutter]/[total] timing lines each run prints, including the
# idle/active/drain phase split within [scheduler] and [cutter].
#
# Usage (run from the esl_proxy/esl_proxy repo root):
#   ./src/benchmark_dedup.sh [N_RUNS] [CASE] [SPMD_TIER]
#
# Defaults: N_RUNS=30, CASE=qwen3_dynamic_tensormap.h, SPMD_TIER=2.
# Note: QWEN3_SPMD_TIER=0 is a known-broken configuration unrelated to
# dedup (a pre-existing fixed-size successor-list overflow in cutter.c);
# avoid it here.

set -u

N=${1:-30}
CASE=${2:-qwen3_dynamic_tensormap.h}
TIER=${3:-2}

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
            printf "median=%.1f mean=%.1f min=%.1f max=%.1f", median, mean, a[1], a[n]
        }')
    echo "$name: $result"
}

measure() {
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

    local init_time=() orch_time=() orch_tp=() sched_dur=() sched_tp=()
    local sched_idle=() sched_active=() sched_drain=()
    local cutter_dur=() cutter_tp=() cutter_idle=() cutter_active=() cutter_drain=()
    local total_time=() total_tp=()

    for i in $(seq 1 "$N"); do
        out=$(WORKER_LOG= ./bin/esl_proxy 2>&1)
        init_time+=("$(echo "$out" | grep -oP '\[init\] elapsed_time = \K[0-9]+')")
        orch_time+=("$(echo "$out" | grep -oP '\[orchestration\] elapsed_time = \K[0-9]+')")
        orch_tp+=("$(echo "$out" | grep -oP '\[orchestration\] task_tp = \K[0-9.]+')")
        sched_dur+=("$(echo "$out" | grep -oP '\[scheduler\] duration = \K[0-9]+')")
        sched_tp+=("$(echo "$out" | grep -oP '\[scheduler\] task_tp = \K[0-9.]+')")
        sched_idle+=("$(echo "$out" | grep -oP '\[scheduler\] idle_ns = \K[0-9]+')")
        sched_active+=("$(echo "$out" | grep -oP '\[scheduler\] active_ns = \K[0-9]+')")
        sched_drain+=("$(echo "$out" | grep -oP '\[scheduler\] drain_ns = \K[0-9]+')")
        cutter_dur+=("$(echo "$out" | grep -oP '\[cutter\] duration = \K[0-9]+')")
        cutter_tp+=("$(echo "$out" | grep -oP '\[cutter\] task_tp = \K[0-9.]+')")
        cutter_idle+=("$(echo "$out" | grep -oP '\[cutter\] idle_ns = \K[0-9]+')")
        cutter_active+=("$(echo "$out" | grep -oP '\[cutter\] active_ns = \K[0-9]+')")
        cutter_drain+=("$(echo "$out" | grep -oP '\[cutter\] drain_ns = \K[0-9]+')")
        total_time+=("$(echo "$out" | grep -oP '\[total\] elapsed_time = \K[0-9]+')")
        total_tp+=("$(echo "$out" | grep -oP '\[total\] task_tp = \K[0-9.]+')")
    done

    echo "=== $label ($N runs, CASE=$CASE, SPMD_TIER=$TIER) ==="
    stats "init elapsed_time (ns)" "${init_time[@]}"
    stats "orchestration elapsed_time (ns)" "${orch_time[@]}"
    stats "orchestration task_tp (MTasks/s)" "${orch_tp[@]}"
    stats "scheduler duration (ns)" "${sched_dur[@]}"
    stats "scheduler task_tp (MTasks/s)" "${sched_tp[@]}"
    stats "scheduler idle_ns (ns)" "${sched_idle[@]}"
    stats "scheduler active_ns (ns)" "${sched_active[@]}"
    stats "scheduler drain_ns (ns)" "${sched_drain[@]}"
    stats "cutter duration (ns)" "${cutter_dur[@]}"
    stats "cutter task_tp (MTasks/s)" "${cutter_tp[@]}"
    stats "cutter idle_ns (ns)" "${cutter_idle[@]}"
    stats "cutter active_ns (ns)" "${cutter_active[@]}"
    stats "cutter drain_ns (ns)" "${cutter_drain[@]}"
    stats "total elapsed_time (ns)" "${total_time[@]}"
    stats "total task_tp (MTasks/s)" "${total_tp[@]}"
    echo ""
}

measure "baseline (no dedup)" ""
measure "dedup enabled" "-DENABLE_DAG_DEDUP"
