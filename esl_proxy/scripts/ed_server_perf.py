#!/usr/bin/env python3
"""
服务器侧 ED 性能复测：4 case × A/B/C × REPEAT，固定 EXEC_DURATION_SCALE。

A  ED_ENABLE=0                         基线（无 ED 代码）
B  ED_ENABLE=1 ED_UNFIN_THRESHOLD=0    ED 代码在场但不 stage
C  ED_ENABLE=1 ED_UNFIN_THRESHOLD=∞    ED 正常工作

输出：tmp/ed_server_perf_<ts>/ 下逐次 CSV + summary.md
"""

from __future__ import annotations

import csv
import math
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    "qwen3_dynamic_manual_scope.h",
    "qwen3_dynamic_tensormap.h",
    "paged_attention_unroll.h",
    "paged_attention_unroll_manual_scope.h",
]
GROUPS = [
    ("A", {"ED_ENABLE": "0"}),
    ("B", {"ED_ENABLE": "1", "ED_UNFIN_THRESHOLD": "0"}),
    ("C", {"ED_ENABLE": "1", "ED_UNFIN_THRESHOLD": "0xFFFF"}),
]
SCALE = sys.argv[1] if len(sys.argv) > 1 else "100"
REPEAT = int(sys.argv[2]) if len(sys.argv) > 2 else 9

PATTERNS = {
    "task_cnt": r"\[orchestration\] task_cnt = (\d+)",
    "completed": r"\[summary\] completed_task_cnt = (\d+)",
    "duration_ns": r"\[scheduler\] duration = (\d+) ns",
    "task_tp": r"\[scheduler\] task_tp = ([0-9.]+) MTasks/s",
    "stage_cnt": r"\[ed\] stage_cnt = (\d+)",
    "hit_cnt": r"\[ed\] hit_cnt = (\d+)",
    "self_notify_cnt": r"\[ed\] self_notify_cnt = (\d+)",
    "gate_open_cnt": r"\[ed\] gate_open_cnt = (\d+)",
    "slot_retry_cnt": r"\[ed\] slot_retry_cnt = (\d+)",
    "leaked_staging": r"\[ed\] leaked_staging = (\d+)",
    "slot_leaked": r"\[ed\] slot_leaked = (\d+)",
    "lat_normal_n": r"\[lat\] normal: samples = (\d+)",
    "lat_normal_mean": r"\[lat\] normal: samples = \d+, mean = ([0-9.]+) ns",
    "lat_normal_p50": r"\[lat\] normal: p50 <= (\d+) ns",
    "lat_ed_n": r"\[lat\] ed: samples = (\d+)",
    "lat_ed_mean": r"\[lat\] ed: samples = \d+, mean = ([0-9.]+) ns",
    "lat_ed_p50": r"\[lat\] ed: p50 <= (\d+) ns",
    "dispatch_rounds": r"\[trace\] dispatch_rounds = (\d+)",
}


def parse(text: str) -> dict:
    row = {}
    for k, pat in PATTERNS.items():
        m = re.search(pat, text)
        if not m:
            row[k] = None
            continue
        row[k] = float(m.group(1)) if "." in m.group(1) else int(m.group(1))
    return row


def build(case: str, flags: dict) -> None:
    args = [
        "make", "-s", "clean",
    ]
    subprocess.run(args, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cmd = [
        "make", "-s", "all",
        f"CASE={case}",
        f"EXEC_DURATION_SCALE={SCALE}",
        "LAT_TRACE=1",
    ] + [f"{k}={v}" for k, v in flags.items()]
    subprocess.run(cmd, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_once() -> dict:
    out = subprocess.run(
        ["./bin/esl_proxy"],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env={"WORKER_LOG": "0", "PATH": "/usr/bin:/bin"},
    )
    return parse(out.stdout)


def median(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def mean(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def cv(vals):
    vals = [v for v in vals if v is not None]
    if len(vals) < 2:
        return None
    m = statistics.mean(vals)
    if m == 0:
        return None
    return statistics.stdev(vals) / m


def fmt(v, digits=0):
    if v is None:
        return "—"
    if isinstance(v, float):
        return f"{v:,.{digits}f}"
    return f"{v:,}"


def main():
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = ROOT / "tmp" / f"ed_server_perf_{ts}"
    out_dir.mkdir(parents=True, exist_ok=True)
    runs_csv = out_dir / "runs.csv"
    summary_md = out_dir / "summary.md"

    fieldnames = [
        "case", "group", "rep", "scale",
        *PATTERNS.keys(),
    ]
    all_rows = []
    print(f"[perf] scale={SCALE} repeat={REPEAT} -> {out_dir}", flush=True)

    with runs_csv.open("w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        for case in CASES:
            for gname, flags in GROUPS:
                print(f"[perf] build {case} {gname} {flags}", flush=True)
                build(case, flags)
                for rep in range(1, REPEAT + 1):
                    t0 = time.time()
                    row = run_once()
                    elapsed = time.time() - t0
                    rec = {
                        "case": case,
                        "group": gname,
                        "rep": rep,
                        "scale": int(SCALE),
                        **row,
                    }
                    writer.writerow(rec)
                    fp.flush()
                    all_rows.append(rec)
                    ok = (
                        row.get("completed") == row.get("task_cnt")
                        and (gname == "A" or row.get("leaked_staging") == 0)
                    )
                    print(
                        f"  {gname} r{rep}: dur={row.get('duration_ns')} "
                        f"n_lat={row.get('lat_normal_mean')} "
                        f"ed_lat={row.get('lat_ed_mean')} "
                        f"stage={row.get('stage_cnt')} "
                        f"{'OK' if ok else 'FAIL'} ({elapsed:.2f}s)",
                        flush=True,
                    )

    # ---- summary ----
    lines = []
    lines.append(f"# ED 服务器性能测试（{ts}）\n")
    lines.append(f"- 机器：Linux server，`EXEC_DURATION_SCALE={SCALE}`，每格重复 {REPEAT} 次取中位数\n")
    lines.append("- A=`ED=0` 基线；B=`ED=1` 但不 stage；C=`ED=1` 正常\n")
    lines.append("- 主 KPI：`ready→runnable` 延迟；makespan 附 CV 供参考\n")

    lines.append("\n## 1) ready→runnable 延迟（中位数，ns）\n")
    lines.append(
        "> 口径警告：`C ed` 只统计 ED 实际生效的那几轮；`C normal` 的样本里已经\n"
        "> **剔除**了被 ED 抢走的任务，因此和 `A normal` 不是同一批任务，\n"
        "> `ed/A`、`Cnormal/A` 都含成分差异，不能当作配对加速比。配对口径请用\n"
        "> `scripts/lat_trace_pair.py`。\n"
    )
    lines.append("| case | A normal | C normal | C ed | ed/A | Cnormal/A | "
                 "覆盖率(生效轮) | ED 生效轮次 |\n")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")

    by = {}
    for r in all_rows:
        by.setdefault((r["case"], r["group"]), []).append(r)

    for case in CASES:
        a = by[(case, "A")]
        c = by[(case, "C")]
        a_n = median([r["lat_normal_mean"] for r in a])
        c_n = median([r["lat_normal_mean"] for r in c])
        # ED 是双峰的：不少轮次一次都没 stage。把这些轮次算进覆盖率会得到 0%，
        # 同时 lat_ed 只能来自生效轮 —— 两列口径必须对齐到同一子集。
        fired = [r for r in c if (r["stage_cnt"] or 0) > 0]
        c_e = median([r["lat_ed_mean"] for r in fired])
        cov = None
        if fired:
            cov = 100.0 * median([r["stage_cnt"] for r in fired]) / (
                median([r["task_cnt"] for r in fired]) or 1)
        ed_ratio = (c_e / a_n) if (a_n and c_e) else None
        n_ratio = (c_n / a_n) if (a_n and c_n) else None
        lines.append(
            f"| `{case}` | {fmt(a_n,1)} | {fmt(c_n,1)} | {fmt(c_e,1)} | "
            f"{fmt(ed_ratio,2)}x | {fmt(n_ratio,2)}x | {fmt(cov,1)}% | "
            f"{len(fired)}/{len(c)} |\n"
        )

    lines.append("\n## 2) Makespan 成本拆解（中位数 duration_ns）\n")
    lines.append("| case | A | B | C | B/A 常驻开销 | C/B 抢槽 | C/A | A的CV | C的CV |\n")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for case in CASES:
        a = [r["duration_ns"] for r in by[(case, "A")]]
        b = [r["duration_ns"] for r in by[(case, "B")]]
        c = [r["duration_ns"] for r in by[(case, "C")]]
        ma, mb, mc = median(a), median(b), median(c)
        lines.append(
            f"| `{case}` | {fmt(ma)} | {fmt(mb)} | {fmt(mc)} | "
            f"{fmt((mb/ma) if ma and mb else None, 2)}x | "
            f"{fmt((mc/mb) if mb and mc else None, 2)}x | "
            f"{fmt((mc/ma) if ma and mc else None, 2)}x | "
            f"{fmt(cv(a), 3)} | {fmt(cv(c), 3)} |\n"
        )

    lines.append("\n## 3) ED 正确性快检（C 组）\n")
    lines.append("| case | doorbell 完整 | leaked_staging | slot_leaked | completed==task |\n")
    lines.append("| --- | ---: | ---: | ---: | ---: |\n")
    for case in CASES:
        rows = by[(case, "C")]
        ok_db = all(
            (r["hit_cnt"] or 0) + (r["self_notify_cnt"] or 0) == (r["stage_cnt"] or 0)
            and (r["gate_open_cnt"] or 0) == (r["stage_cnt"] or 0)
            for r in rows
        )
        leak = max(r["leaked_staging"] or 0 for r in rows)
        slot = max(r["slot_leaked"] or 0 for r in rows)
        done = all(r["completed"] == r["task_cnt"] for r in rows)
        lines.append(
            f"| `{case}` | {'YES' if ok_db else 'NO'} | {leak} | {slot} | {'YES' if done else 'NO'} |\n"
        )

    lines.append("\n## 4) 读法与已知失真\n")
    lines.append("- `ed/A < 1`：ED 路径相对基线变快（越小越好），但含成分差异，见上方口径警告\n")
    lines.append("- `Cnormal/A`：开 ED 后 normal 路径是否被拖慢\n")
    lines.append("- `B/A`：ED 常驻开销；目标接近 1.0\n")
    lines.append("- CV 是同一二进制内部的波动，不代表 B/A 比值的可复现性：\n")
    lines.append("  实测同一配置在不同轮次间 B/A 可差 ±0.2x，结论粒度不要细于 0.3x\n")
    lines.append("- `LAT_TRACE=1` 自身会改变 makespan（实测最大 ±0.5x 影响 B/A），\n")
    lines.append("  比 makespan 时应另跑一组 `LAT_TRACE=0`（见 `scripts/ed_overhead_probe.py`）\n")
    lines.append(f"- `EXEC_DURATION_SCALE={SCALE}` 时单次运行仅数毫秒，建议用 scale=10 复核\n")

    summary_md.write_text("".join(lines))
    print("\n" + "".join(lines), flush=True)
    print(f"[perf] wrote {runs_csv}", flush=True)
    print(f"[perf] wrote {summary_md}", flush=True)


if __name__ == "__main__":
    main()
