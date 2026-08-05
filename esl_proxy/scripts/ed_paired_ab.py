#!/usr/bin/env python3
"""两个已编译二进制的配对 A/B 检验。

为什么需要它：ed_overhead_probe.py 是「块设计」——先把 A 跑 N 次，再把 B 跑
N 次。机器的慢漂移（别人的编译任务、CPU 降频、页缓存状态）会整块地压在某
一个 variant 上。实测同一个二进制放在首尾两个位置，中位数能差 6%，而我们要
判断的效应量本身只有 10%~15%，块设计不够用。

这里改成配对设计：每一对里两个二进制紧挨着跑，且用 ABBA 交替消除「对内先跑
的那个占便宜/吃亏」。漂移在一对之内几乎为零，所以每对的比值是干净的。

主要结论看两个数：
  - 配对比值的中位数：效应量
  - 符号检验：B 比 A 慢的对数占比。真效应会明显偏离 50%；纯噪声则接近 50%。

用法:
  ed_paired_ab.py <binA> <binB> [pairs]
"""

from __future__ import annotations

import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DURATION_RE = re.compile(r"\[scheduler\] duration = (\d+) ns")
ENV = {"WORKER_LOG": "0", "PATH": "/usr/bin:/bin"}


def run(binary: str) -> int:
    out = subprocess.run([binary], cwd=ROOT, check=True, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=ENV).stdout
    m = DURATION_RE.search(out)
    if not m:
        sys.exit(f"[paired] 解析不到 duration，binary={binary}")
    return int(m.group(1))


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bin_a, bin_b = sys.argv[1], sys.argv[2]
    pairs = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    if os.getloadavg()[0] > 3.0:
        sys.exit(f"[paired] 中止：load1={os.getloadavg()[0]:.1f}，机器不干净")

    run(bin_a)  # 预热：把页缓存和 CPU 频率带到稳态，不计入统计
    run(bin_b)

    a_list, b_list, ratios = [], [], []
    for i in range(pairs):
        if i % 2 == 0:            # ABBA：偶数对先 A，奇数对先 B
            a, b = run(bin_a), run(bin_b)
        else:
            b, a = run(bin_b), run(bin_a)
        a_list.append(a)
        b_list.append(b)
        ratios.append(b / a)

    slower = sum(1 for r in ratios if r > 1.0)
    print(f"配对数        = {pairs}")
    print(f"A 中位数      = {statistics.median(a_list):>13,.0f} ns   "
          f"({bin_a})")
    print(f"B 中位数      = {statistics.median(b_list):>13,.0f} ns   "
          f"({bin_b})")
    print(f"配对比值中位数 = {statistics.median(ratios):.4f}x")
    print(f"配对比值均值   = {statistics.fmean(ratios):.4f}x  "
          f"(标准差 {statistics.stdev(ratios):.4f})")
    print(f"符号检验      = {slower}/{pairs} 对里 B 更慢 "
          f"({100.0 * slower / pairs:.0f}%)，纯噪声应约 50%")


if __name__ == "__main__":
    main()
