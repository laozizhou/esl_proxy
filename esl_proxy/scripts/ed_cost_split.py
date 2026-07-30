#!/usr/bin/env python3
"""
把 ED 带来的 normal 路径延迟恶化拆成两部分：固定开销 vs 占槽效应。

三组构建（其余参数完全一致）：
  A  ED_ENABLE=0                        无 ED 代码，基线
  B  ED_ENABLE=1 ED_UNFIN_THRESHOLD=0   ED 代码与开销全在，但几乎不 stage
  C  ED_ENABLE=1 ED_UNFIN_THRESHOLD=许可 ED 正常工作

B - A 是 ED 的固定开销，C - B 是提前占槽本身的影响。
"""

import re
import statistics
import subprocess
import sys

CASE = sys.argv[1] if len(sys.argv) > 1 else "paged_attention_unroll_manual_scope.h"
REPEAT = int(sys.argv[2]) if len(sys.argv) > 2 else 9

GROUPS = [
    ("A ED=0 基线", {"ED_ENABLE": "0"}),
    ("B ED=1 不stage", {"ED_ENABLE": "1", "ED_UNFIN_THRESHOLD": "0"}),
    ("C ED=1 正常", {"ED_ENABLE": "1", "ED_UNFIN_THRESHOLD": "0xFFFF"}),
]

PATTERNS = {
    "duration_ns": r"\[scheduler\] duration = (\d+) ns",
    "stage_cnt": r"\[ed\] stage_cnt = (\d+)",
    "normal_n": r"\[lat\] normal: samples = (\d+)",
    "normal_mean": r"\[lat\] normal: samples = \d+, mean = ([\d.]+) ns",
    "ed_n": r"\[lat\] ed: samples = (\d+)",
    "ed_mean": r"\[lat\] ed: samples = \d+, mean = ([\d.]+) ns",
    "rounds": r"\[trace\] dispatch_rounds = (\d+)",
    "cube_starve_pct": r"\[trace\] cube: starve = \d+ \(([\d.]+)%\)",
    "cube_free_mean": r"\[trace\] cube: calls = \d+, free_core_mean = ([\d.]+)",
    "slot0": r"\[trace\] cube: free slot0 = ([\d.]+)",
    "slot1": r"\[trace\] cube: free slot0 = [\d.]+, slot1 = ([\d.]+)",
    "and_cores": r"\[trace\] cube: free slot0 = [\d.]+, slot1 = [\d.]+, AND = ([\d.]+)",
    "or_cores": r"\[trace\] cube: free slot0 = [\d.]+, slot1 = [\d.]+, AND = [\d.]+, OR = ([\d.]+)",
}


def run_group(label, flags):
    args = [f"{k}={v}" for k, v in flags.items()]
    subprocess.run(["make", "-s", "clean"], check=True)
    subprocess.run(["make", "-s", "all", f"CASE={CASE}", "LAT_TRACE=1"] + args,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    rows = []
    for _ in range(REPEAT):
        out = subprocess.run(["./bin/esl_proxy"], check=True, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             env={"WORKER_LOG": "0", "PATH": "/usr/bin:/bin"}).stdout
        row = {}
        for key, pat in PATTERNS.items():
            m = re.search(pat, out)
            row[key] = float(m.group(1)) if m else None
        rows.append(row)
    return rows


def med(rows, key):
    vals = [r[key] for r in rows if r[key] is not None]
    return statistics.median(vals) if vals else None


def fmt(v, digits=0):
    return "—" if v is None else f"{v:,.{digits}f}"


def main():
    print(f"case = {CASE}, repeat = {REPEAT}（每格为中位数）\n")
    results = {}
    for label, flags in GROUPS:
        rows = run_group(label, flags)
        results[label] = rows
        print(f"[{label}] 逐次 normal_mean_ns: "
              f"{[int(r['normal_mean']) if r['normal_mean'] else None for r in rows]}")
        print(f"[{label}] 逐次 stage_cnt:      "
              f"{[int(r['stage_cnt']) if r['stage_cnt'] is not None else '-' for r in rows]}")

    hdr = ["组", "stage_cnt", "normal_n", "normal_mean_ns", "ed_mean_ns",
           "makespan_ns", "starve%", "slot0空", "slot1空", "AND可用", "OR可用"]
    print("\n" + " | ".join(f"{h:>15}" for h in hdr))
    print("-" * (18 * len(hdr)))
    for label, _ in GROUPS:
        rows = results[label]
        cells = [
            label,
            fmt(med(rows, "stage_cnt")),
            fmt(med(rows, "normal_n")),
            fmt(med(rows, "normal_mean")),
            fmt(med(rows, "ed_mean")),
            fmt(med(rows, "duration_ns")),
            fmt(med(rows, "cube_starve_pct"), 2),
            fmt(med(rows, "slot0"), 2),
            fmt(med(rows, "slot1"), 2),
            fmt(med(rows, "and_cores"), 2),
            fmt(med(rows, "or_cores"), 2),
        ]
        print(" | ".join(f"{c:>15}" for c in cells))

    a = med(results[GROUPS[0][0]], "normal_mean")
    b = med(results[GROUPS[1][0]], "normal_mean")
    c = med(results[GROUPS[2][0]], "normal_mean")
    if None not in (a, b, c):
        print(f"\nnormal 路径延迟拆解（中位数，单位 ns）:")
        print(f"  A 基线                    {a:,.0f}")
        print(f"  B - A  ED 固定开销贡献     {b - a:+,.0f}   ({b / a:.2f}x)")
        print(f"  C - B  提前占槽额外贡献     {c - b:+,.0f}   ({c / b:.2f}x)")
        print(f"  C / A  合计恶化            {c / a:.2f}x")


if __name__ == "__main__":
    main()
