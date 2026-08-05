#!/usr/bin/env python3
"""把 ed_layout_sweep.py 的日志换算成两个不同的问题的答案。

问题一「ED 到底值多少」：要的是把布局抽签平均掉之后的数，即两组变体
中位数的中位数之比。这个数跨机器口径一致，是评价 ED 的依据。

问题二「我随机编一次跑一次会看到什么」：要的是分布。随便抽一个 A 变体和一个
B 变体作比，是 16x16=256 种组合，输出它的分位数。这个数说明单次测量有多不可信。

两者差别巨大：同一份数据，问题一可能是 1.01，问题二的区间可能横跨 0.85~1.25。

用法: ed_sweep_report.py <sweep 日志> [...]
"""

from __future__ import annotations

import re
import statistics as st
import sys
from pathlib import Path

CASE_RE = re.compile(r"^#+ CASE=(\S+)")
SWEEP_RE = re.compile(r"^\[sweep\] case=(\S+)")
CFG_RE = re.compile(r"^\s{2}(\S+)\s+变体中位数")
VAR_RE = re.compile(r"^\s+pad=(\d+)\s+帧=(\d+)\s+页内偏移=(\d+)\s+中位数=\s*([\d,]+)")


def parse(paths: list[str]) -> dict[str, dict[str, list[float]]]:
    cases: dict[str, dict[str, list[float]]] = {}
    case = cfg = None
    for p in paths:
        for line in Path(p).read_text().splitlines():
            m = CASE_RE.match(line) or SWEEP_RE.match(line)
            if m:
                case = m.group(1)
                cases.setdefault(case, {})
                continue
            m = CFG_RE.match(line)
            if m:
                cfg = m.group(1)
                cases[case][cfg] = []
                continue
            m = VAR_RE.match(line)
            if m and case and cfg:
                cases[case][cfg].append(float(m.group(4).replace(",", "")))
    return {k: v for k, v in cases.items() if any(v.values())}


def pct(xs: list[float], q: float) -> float:
    s = sorted(xs)
    i = q * (len(s) - 1)
    lo = int(i)
    return s[lo] if lo + 1 >= len(s) else s[lo] + (s[lo + 1] - s[lo]) * (i - lo)


def draws(num: list[float], den: list[float]) -> list[float]:
    """随机抽一个 num 变体、一个 den 变体，两两作比的全部组合。"""
    return [a / b for a in num for b in den]


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cases = parse(sys.argv[1:])
    for case, cfgs in cases.items():
        n = {k: len(v) for k, v in cfgs.items()}
        if not {"A", "B", "C"} <= cfgs.keys() or min(n.values()) < 4:
            print(f"\n##### {case}: 变体数不足，跳过 {n}")
            continue
        A, B, C = cfgs["A"], cfgs["B"], cfgs["C"]
        print(f"\n##### {case}  (每配置 {len(A)} 个变体)")

        print("\n  [口径一] 平均掉布局抽签后 ED 的真实账（中位数的中位数之比）")
        mA, mB, mC = st.median(A), st.median(B), st.median(C)
        print(f"    常驻开销   B/A = {mB/mA:.4f}x  ({100*(mB/mA-1):+.1f}%)  "
              f"normal 路径为开 ED 付的代价")
        print(f"    机制毛收益 C/B = {mC/mB:.4f}x  ({100*(mC/mB-1):+.1f}%)  "
              f"stage 生效带来的加速")
        print(f"    端到端净值 C/A = {mC/mA:.4f}x  ({100*(mC/mA-1):+.1f}%)  "
              f"上面两笔相抵之后")

        print("\n  [口径二] 随机编一次跑一次会看到什么（256 种变体组合的分布）")
        for name, num, den in (("B/A", B, A), ("C/A", C, A)):
            d = draws(num, den)
            worse = 100.0 * sum(1 for x in d if x > 1.0) / len(d)
            print(f"    {name}: p10={pct(d,.1):.3f} p50={pct(d,.5):.3f} "
                  f"p90={pct(d,.9):.3f}  区间=[{min(d):.3f}, {max(d):.3f}]  "
                  f"抽到\"变慢\"的概率={worse:.0f}%")

        print("\n  [判据] 两组分布是否分离（分离才能说开销与布局无关）")
        for name, x, y in (("B vs A", B, A), ("C vs A", C, A)):
            ov = sum(1 for v in x if min(y) <= v <= max(y))
            sep = "分离，开销是真的" if ov == 0 else (
                f"重叠 {ov}/{len(x)}，布局抽签足以解释，不能归因给 ED")
            print(f"    {name}: {sep}")
        print(f"    各组内部极差（纯布局造成）: "
              + "  ".join(f"{k}={100*(max(v)/min(v)-1):.0f}%" for k, v in
                          (("A", A), ("B", B), ("C", C))))


if __name__ == "__main__":
    main()
