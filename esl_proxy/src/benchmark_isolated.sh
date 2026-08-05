#!/bin/bash
# benchmark_isolated.sh - baseline vs dedup comparison for the isolated
# orch_only / sched_only test harnesses.
#
# Unlike benchmark_dedup.sh, this does NOT rebuild between variants: each
# variant here is already a separate, pre-built binary (orch_only_baseline
# vs orch_only_dedup; sched_only fed dag_baseline.csv vs dag_dedup.csv), so
# every run alternates baseline/dedup directly with no compile step in
# between -- cheaper and simpler than the round-based interleaving
# benchmark_dedup.sh needs.
#
# Usage (run from the esl_proxy/esl_proxy repo root, after building the
# orch_only baseline/dedup binaries, the shared sched_only binary, and
# generating the matching baseline/dedup CSVs via dag_export):
#   ./src/benchmark_isolated.sh [N_RUNS] [ORCH_BASE_BIN] [ORCH_DEDUP_BIN] [CSV_BASE] [CSV_DEDUP]
#
# Defaults match the original qwen3_dynamic_tensormap.h TIER=2 setup:
#   N_RUNS=200
#   ORCH_BASE_BIN=bin/orch_only_baseline
#   ORCH_DEDUP_BIN=bin/orch_only_dedup
#   CSV_BASE=dag_csv/dag_baseline.csv
#   CSV_DEDUP=dag_csv/dag_dedup.csv
#
# sched_only itself is case-agnostic (reads whatever CSV it's given), so
# there's no separate binary to parameterize for it -- always bin/sched_only.

set -u

N=${1:-200}
ORCH_BASE_BIN=${2:-bin/orch_only_baseline}
ORCH_DEDUP_BIN=${3:-bin/orch_only_dedup}
CSV_BASE=${4:-dag_csv/dag_baseline.csv}
CSV_DEDUP=${5:-dag_csv/dag_dedup.csv}

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

echo "=== orch_only (N=$N, interleaved every run) ==="
orch_base_time=()
orch_base_tp=()
orch_dedup_time=()
orch_dedup_tp=()
for i in $(seq 1 "$N"); do
    out=$("$ORCH_BASE_BIN" 2>&1)
    orch_base_time+=("$(echo "$out" | grep -oP '\[orchestration\] elapsed_time = \K[0-9]+')")
    orch_base_tp+=("$(echo "$out" | grep -oP '\[orchestration\] task_tp = \K[0-9.]+')")

    out=$("$ORCH_DEDUP_BIN" 2>&1)
    orch_dedup_time+=("$(echo "$out" | grep -oP '\[orchestration\] elapsed_time = \K[0-9]+')")
    orch_dedup_tp+=("$(echo "$out" | grep -oP '\[orchestration\] task_tp = \K[0-9.]+')")
done
stats "orch_only baseline elapsed_time (ns)" "${orch_base_time[@]}"
stats "orch_only baseline task_tp (MTasks/s)" "${orch_base_tp[@]}"
stats "orch_only dedup elapsed_time (ns)" "${orch_dedup_time[@]}"
stats "orch_only dedup task_tp (MTasks/s)" "${orch_dedup_tp[@]}"
echo ""

echo "=== sched_only (N=$N, interleaved every run) ==="
sched_base_time=()
sched_base_tp=()
sched_dedup_time=()
sched_dedup_tp=()
sched_base_missing=0
sched_dedup_missing=0
for i in $(seq 1 "$N"); do
    out=$(./bin/sched_only "$CSV_BASE" 2>&1)
    sched_base_time+=("$(echo "$out" | grep -oP '\[sched_only\] elapsed_time = \K[0-9]+')")
    sched_base_tp+=("$(echo "$out" | grep -oP '\[sched_only\] task_tp = \K[0-9.]+')")
    sc=$(echo "$out" | grep -oP '\[scheduler\] task_cnt = \K[0-9]+')
    ct=$(echo "$out" | grep -oP '\[cutter\] task_cnt = \K[0-9]+')
    if [ "$sc" != "$ct" ]; then
        sched_base_missing=$((sched_base_missing + 1))
    fi

    out=$(./bin/sched_only "$CSV_DEDUP" 2>&1)
    sched_dedup_time+=("$(echo "$out" | grep -oP '\[sched_only\] elapsed_time = \K[0-9]+')")
    sched_dedup_tp+=("$(echo "$out" | grep -oP '\[sched_only\] task_tp = \K[0-9.]+')")
    sc=$(echo "$out" | grep -oP '\[scheduler\] task_cnt = \K[0-9]+')
    ct=$(echo "$out" | grep -oP '\[cutter\] task_cnt = \K[0-9]+')
    if [ "$sc" != "$ct" ]; then
        sched_dedup_missing=$((sched_dedup_missing + 1))
    fi
done
stats "sched_only baseline elapsed_time (ns)" "${sched_base_time[@]}"
stats "sched_only baseline task_tp (MTasks/s)" "${sched_base_tp[@]}"
echo "sched_only baseline missing-task-count occurrences: $sched_base_missing / $N"
stats "sched_only dedup elapsed_time (ns)" "${sched_dedup_time[@]}"
stats "sched_only dedup task_tp (MTasks/s)" "${sched_dedup_tp[@]}"
echo "sched_only dedup missing-task-count occurrences: $sched_dedup_missing / $N"
