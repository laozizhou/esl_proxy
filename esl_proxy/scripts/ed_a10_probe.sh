#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_DIR="${LOG_DIR:-log/a10_$(date +%Y%m%d_%H%M%S)}"
REPEAT="${REPEAT:-5}"
FORCE_SELF_REPEAT="${FORCE_SELF_REPEAT:-2}"
mkdir -p "$LOG_DIR"

CASES=(qwen3_dynamic_manual_scope.h paged_attention_unroll.h)

for c in "${CASES[@]}"; do
    for r in $(seq 1 "$REPEAT"); do
        make -s clean >/dev/null 2>&1
        make -s all CASE="$c" ED_ENABLE=1 >/dev/null 2>&1
        WORKER_LOG=1 ./bin/esl_proxy >"$LOG_DIR/${c%.h}_r${r}.log" 2>&1
    done
done

for r in $(seq 1 "$FORCE_SELF_REPEAT"); do
    make -s clean >/dev/null 2>&1
    make -s all CASE=ed_a10_probe.h ED_ENABLE=1 \
        EXTRA_CFLAGS="-DED_A10_FORCE_SELF_NOTIFY=1" >/dev/null 2>&1
    WORKER_LOG=1 ./bin/esl_proxy >"$LOG_DIR/ed_a10_probe_selfforce_r${r}.log" 2>&1
done

python3 - "$LOG_DIR" <<'PY'
import glob
import re
import sys
from collections import Counter

log_dir = sys.argv[1]
files = sorted(glob.glob(f"{log_dir}/*.log"))
assert files, "A10 fail: no logs generated"

any_hit = False
any_self = False
errors = []

for f in files:
    text = open(f, "r", encoding="utf-8", errors="ignore").read()
    hit_m = re.search(r"\[ed\] hit_cnt = (\d+)", text)
    self_m = re.search(r"\[ed\] self_notify_cnt = (\d+)", text)
    stage_m = re.search(r"\[ed\] stage_cnt = (\d+)", text)
    if not (hit_m and self_m and stage_m):
        errors.append(f"A10 metric missing: {f}")
        continue

    hit = int(hit_m.group(1))
    self_ = int(self_m.group(1))
    stage = int(stage_m.group(1))
    if hit + self_ != stage:
        errors.append(f"A10.3 fail: {f} hit={hit} self={self_} stage={stage}")
    if hit > 0:
        any_hit = True
    if self_ > 0:
        any_self = True

    cnt = Counter()
    for line in text.splitlines():
        m = re.search(r"notify_write, s=(\d+)", line)
        if m:
            cnt[m.group(1)] += 1
    bad = [(task, n) for task, n in cnt.items() if n != 1]
    if bad:
        errors.append(f"A10.4 fail: {f} {bad[:5]}")

    for line in text.splitlines():
        m = re.search(r"slot_free, task=(\d+), tag=(\d+)", line)
        if not m:
            continue
        task = int(m.group(1))
        tag = int(m.group(2))
        if task != tag:
            errors.append(f"A10.5 fail: {f} slot_free task={task} tag={tag}")

if not any_hit:
    errors.append("A10.1 fail: no Hook2 notify observed")
if not any_self:
    errors.append("A10.2 fail: no Hook1 self-notify observed")

if errors:
    raise SystemExit("\n".join(errors))

print("A10 PASS")
PY
