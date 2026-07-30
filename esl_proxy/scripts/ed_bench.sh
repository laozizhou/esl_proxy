#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)

REPEAT="${REPEAT:-5}"
LOG_DIR="${LOG_DIR:-log/ed_bench_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$LOG_DIR"

for case_h in "${CASES[@]}"; do
    for ed in 0 1; do
        for rep in $(seq 1 "$REPEAT"); do
            log_file="$LOG_DIR/${case_h%.h}_ed${ed}_r${rep}.log"
            echo "[bench] case=$case_h ed=$ed rep=$rep -> $log_file"
            make -s clean >/dev/null 2>&1
            make -s all CASE="$case_h" ED_ENABLE="$ed" >/dev/null 2>&1
            WORKER_LOG=1 ./bin/esl_proxy >"$log_file" 2>&1
        done
    done
done

python3 scripts/ed_bench_summary.py "$LOG_DIR" >"$LOG_DIR/summary.csv"
cat "$LOG_DIR/summary.csv"
echo "[bench] summary => $LOG_DIR/summary.csv"
