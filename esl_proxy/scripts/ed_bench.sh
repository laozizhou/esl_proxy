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
# WORKER_LOG=1 是 A3（notify_write 逐条计数）的前提，但会显著拖慢运行并产生
# 大量日志；只测性能/延迟 KPI 时设为 0。
BENCH_WORKER_LOG="${BENCH_WORKER_LOG:-1}"
mkdir -p "$LOG_DIR"

for case_h in "${CASES[@]}"; do
    for ed in 0 1; do
        # 每个 (case, ed) 组合只构建一次：配置不变时重复 clean 既慢又无必要，
        # 而配置变化时必须 clean（ORCH_STAMP 在构建被中断后不可靠）。
        make -s clean >/dev/null 2>&1
        make -s all CASE="$case_h" ED_ENABLE="$ed" >/dev/null 2>&1
        for rep in $(seq 1 "$REPEAT"); do
            log_file="$LOG_DIR/${case_h%.h}_ed${ed}_r${rep}.log"
            echo "[bench] case=$case_h ed=$ed rep=$rep -> $log_file"
            # 记下 worker_log 开关，summary 据此决定是否跳过 A3（依赖逐条日志）
            echo "[bench] worker_log=$BENCH_WORKER_LOG" >"$log_file"
            WORKER_LOG="$BENCH_WORKER_LOG" ./bin/esl_proxy >>"$log_file" 2>&1
        done
    done
done

python3 scripts/ed_bench_summary.py "$LOG_DIR" >"$LOG_DIR/summary.csv"
cat "$LOG_DIR/summary.csv"
echo "[bench] summary => $LOG_DIR/summary.csv"
