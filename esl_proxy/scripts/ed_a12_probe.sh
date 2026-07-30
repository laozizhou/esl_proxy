#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_DIR="${LOG_DIR:-log/a12_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$LOG_DIR"

# 场景 1: 常规图验证 fanin 汇总等式
A12_CASE="${A12_CASE:-qwen3_dynamic_tensormap_ot1.h}"
make -s clean >/dev/null 2>&1
if ! make -s all CASE="$A12_CASE" ED_ENABLE=1 EXTRA_CFLAGS="-DED_HOOK0_CONTRIB_STATS=1" >/dev/null 2>&1; then
    A12_CASE="qwen3_dynamic_tensormap.h"
    make -s clean >/dev/null 2>&1
    make -s all CASE="$A12_CASE" ED_ENABLE=1 EXTRA_CFLAGS="-DED_HOOK0_CONTRIB_STATS=1" >/dev/null 2>&1
fi
WORKER_LOG=1 ./bin/esl_proxy >"$LOG_DIR/a12_qwen3.log" 2>&1

python3 - "$LOG_DIR/a12_qwen3.log" <<'PY'
import re
import sys

txt = open(sys.argv[1], "r", encoding="utf-8", errors="ignore").read()
la_m = re.search(r"\[ed\] late_arrival_cnt = (\d+)", txt)
h0_m = re.search(r"\[ed\] hook0_contrib_cnt = (\d+)", txt)
tgt_m = re.search(r"\[ed\] sum_fanin_target = (\d+)", txt)
if not (la_m and h0_m and tgt_m):
    raise SystemExit("A12 fail: missing late_arrival_cnt/hook0_contrib_cnt/sum_fanin_target")

late = int(la_m.group(1))
h0 = int(h0_m.group(1))
tgt = int(tgt_m.group(1))
mismatches = re.findall(r"\[ed\] fanin_check, .*MISMATCH", txt)
assert not mismatches, f"A12.1 fail: {mismatches[:5]}"
assert late + h0 == tgt, f"A12.3 fail: late={late} hook0={h0} tgt={tgt}"
print("A12.1/A12.3 PASS")
PY

# 场景 2: ring 卷绕与 stale_tag 证据
make -s clean >/dev/null 2>&1
make -s all CASE=paged_attention_unroll_manual_scope.h ED_ENABLE=1 >/dev/null 2>&1
WORKER_LOG=1 ./bin/esl_proxy >"$LOG_DIR/a12_ring.log" 2>&1
if ! awk '/stale_tag/{seen=1} END{exit(seen?0:1)}' "$LOG_DIR/a12_ring.log"; then
    echo "[A12] baseline case 未触发 stale_tag，回退到 stress case"
    make -s clean >/dev/null 2>&1
    make -s all CASE=ed_a12_ring_stress.h ED_ENABLE=1 >/dev/null 2>&1
    WORKER_LOG=1 ./bin/esl_proxy >"$LOG_DIR/a12_ring.log" 2>&1
fi

awk '
/stale_tag/ {seen=1}
/notify_write.*tag_mismatch/ {print "FAIL A12.4", $0; f++}
END {
    if (!seen) { print "FAIL A12.4 no stale_tag evidence"; f++; }
    exit (f ? 1 : 0);
}' "$LOG_DIR/a12_ring.log"

echo "A12 PASS"
