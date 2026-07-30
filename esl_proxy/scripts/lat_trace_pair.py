#!/usr/bin/env python3
"""
按 task_id 配对比较 LAT_TRACE 产出的逐任务延迟分解。

用法:
    lat_trace_pair.py <ed1.csv> <ed0.csv> [<ed0.csv> ...]

多份 ED=0 CSV 时逐任务取中位数，用来压掉运行间抖动。

分组依据 ED=1 那次运行的路径归属：
    ED 组      —— ED=1 时被门铃放行的任务
    normal 组  —— ED=1 时仍走常规派发的任务
两组分别与 ED=0 下同一批 task_id 对比，回答两个问题：
    1. ED 组真的被提前放行了吗（相对它自己在 ED=0 下的等待）
    2. normal 组在 ED=1 下变慢了吗，慢在哪一段
"""

import csv
import statistics
import sys

SEGMENTS = [
    ("d_ready_enq", "ready->enq  cutter 攒批入队"),
    ("d_enq_deq", "enq->deq    队列里等 dispatcher 取"),
    ("d_deq_run", "deq->run    出队后写槽位"),
    ("d_total", "ready->run  合计"),
]


def load(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["task_id"])] = {k: int(v) for k, v in r.items()}
    return out


def median_runs(runs, task_id, field):
    vals = [r[task_id][field] for r in runs if task_id in r and r[task_id][field] >= 0]
    return statistics.median(vals) if vals else None


def describe(label, values):
    if not values:
        return f"  {label:<34} 无样本"
    values = sorted(values)
    n = len(values)
    return (
        f"  {label:<34} n={n:<5} "
        f"mean={statistics.mean(values):>10.0f}  "
        f"p50={values[n // 2]:>9.0f}  "
        f"p90={values[min(n - 1, int(n * 0.9))]:>9.0f}  "
        f"max={values[-1]:>10.0f}"
    )


def compare_group(name, task_ids, ed1, ed0_runs):
    print(f"\n=== {name}（{len(task_ids)} 个任务）===")
    for field, desc in SEGMENTS:
        a = [ed1[t][field] for t in task_ids if ed1[t][field] >= 0]
        b = [v for t in task_ids if (v := median_runs(ed0_runs, t, field)) is not None]
        print(f"\n{desc}")
        print(describe("ED=1", a))
        print(describe("ED=0 (逐任务中位数)", b))
        if a and b:
            ratio = statistics.mean(a) / statistics.mean(b) if statistics.mean(b) else float("inf")
            print(f"  {'ED=1 / ED=0 均值倍数':<34} {ratio:.2f}x")

    # 逐任务差值：同一个任务在两种构建下的合计延迟之差
    deltas = []
    for t in task_ids:
        if ed1[t]["d_total"] < 0:
            continue
        base = median_runs(ed0_runs, t, "d_total")
        if base is not None:
            deltas.append(ed1[t]["d_total"] - base)
    if deltas:
        worse = sum(1 for d in deltas if d > 0)
        print(f"\n  逐任务配对差值 (ED=1 - ED=0) n={len(deltas)}")
        print(f"  {'变慢的任务占比':<34} {100.0 * worse / len(deltas):.1f}%")
        print(f"  {'差值 mean / p50':<34} "
              f"{statistics.mean(deltas):.0f} / {sorted(deltas)[len(deltas) // 2]:.0f} ns")


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    ed1 = load(sys.argv[1])
    ed0_runs = [load(p) for p in sys.argv[2:]]

    common = [t for t in ed1 if all(t in r for r in ed0_runs)]
    ed_group = sorted(t for t in common if ed1[t]["path"] == 2)
    normal_group = sorted(t for t in common if ed1[t]["path"] == 1)

    print(f"ED=1 文件: {sys.argv[1]}")
    print(f"ED=0 文件: {', '.join(sys.argv[2:])}")
    print(f"两侧都有记录的任务数: {len(common)}"
          f"  (ED 路径 {len(ed_group)} / normal 路径 {len(normal_group)})")

    if normal_group:
        compare_group("ED=1 下走 normal 路径的任务", normal_group, ed1, ed0_runs)
    if ed_group:
        compare_group("ED=1 下走 ED 路径的任务", ed_group, ed1, ed0_runs)

    # 出队次数：ED 任务同样会被 dispatcher 出队再 skip，属于被浪费的名额
    deq_ed = [ed1[t]["deq_cnt"] for t in ed_group]
    deq_no = [ed1[t]["deq_cnt"] for t in normal_group]
    if deq_ed or deq_no:
        print("\n=== ED=1 出队次数（每次出队都占一个派发名额）===")
        if deq_ed:
            print(f"  ED 路径任务 deq_cnt   总计 {sum(deq_ed)}，均值 {statistics.mean(deq_ed):.2f}")
        if deq_no:
            print(f"  normal 路径任务 deq_cnt 总计 {sum(deq_no)}，均值 {statistics.mean(deq_no):.2f}")


if __name__ == "__main__":
    main()
