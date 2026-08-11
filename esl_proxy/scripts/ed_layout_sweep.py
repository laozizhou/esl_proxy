#!/usr/bin/env python3
"""跨「栈区页内偏移」的分布式 makespan 测量。

===== 为什么需要它 =====

makespan 对「调度器热调用链的栈区页内偏移」（即栈地址的低 12 位）极度敏感：
实测偏移为 0（偏移量是 4096 的整数倍）时无代价，任何非整页偏移一律慢 10% 左右。
而开关 ED_ENABLE 必然改变热函数的栈帧大小（dispatch +16、send_task +16、
resolve_dep +16、add_successors +48、executor_worker +32），于是 B/A 里混进了
一笔与 ED 逻辑完全无关、量级却和 ED 本身相当（约 10%）的账。

后果是单点测量根本无法回答「ED 多花了多少」：
  A vs B                       = 1.08x
  A 补 16 字节栈 vs B          = 0.99x   <- 对照组一行 ED 代码都不执行
  A 补 32 字节栈 vs B          = 0.96x   <- 同上，B 反而更快
三个数都是同一台机器同一个用例，差别只在给 A 补了几个字节的栈。

===== 做法 =====

把隐藏变量变成随机变量再平均掉：每个配置编 N 个变体，彼此只有
ED_STACK_PAD_BYTES 不同，让页内偏移大致均匀铺满一页；每个变体跑若干次取中位数，
最后比较两个配置的「中位数集合」的分布，而不是比较两个点。

这样得到的比值不依赖于「本次编译恰好抽到哪个页内偏移」，跨机器、跨编译器版本
都是同一套口径。代价是编译和测量次数各乘 N。

===== 用法 =====

  ed_layout_sweep.py <case> <scale> <runs> <pad_cnt> <name:K=V,...> [...]

例：
  ed_layout_sweep.py qwen3_dynamic_manual_scope.h 10 31 16 \\
      "A:ED_ENABLE=0" \\
      "B:ED_ENABLE=1,ED_UNFIN_THRESHOLD=0" \\
      "C:ED_ENABLE=1,ED_UNFIN_THRESHOLD=0xFFFF"
"""

from __future__ import annotations

import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DURATION_RE = re.compile(r"\[scheduler\] duration = (\d+) ns")
TASK_RE = re.compile(r"\[orchestration\] task_cnt = (\d+)")
DONE_RE = re.compile(r"\[scheduler\] task_cnt = (\d+)")
ENV = {"WORKER_LOG": "0", "PATH": "/usr/bin:/bin"}
MAX_LOAD1 = float(os.environ.get("PROBE_MAX_LOAD", "3.0"))

# 栈填充的最小值。取 8 而不是「不定义」，是为了让扫描里每个变体都含有那条
# volatile 语句 —— 这样整组变体之间唯一的差别就是栈帧大小，不掺入别的代码差异。
PAD_BASE = 8


def wait_quiet() -> None:
    while os.getloadavg()[0] > MAX_LOAD1:
        print(f"[sweep] 等待机器空闲 load1={os.getloadavg()[0]:.1f}", flush=True)
        time.sleep(15)


def build(case: str, scale: str, flags: dict, pad: int) -> None:
    subprocess.run(["make", "-s", "clean"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    extra = flags.get("EXTRA_CFLAGS", "")
    extra = f"{extra} -DED_STACK_PAD_BYTES={pad}".strip()
    cmd = ["make", "-s", "all", f"CASE={case}",
           f"EXEC_DURATION_SCALE={scale}", "LAT_TRACE=0"]
    cmd += [f"{k}={v}" for k, v in flags.items() if k != "EXTRA_CFLAGS"]
    cmd.append(f"EXTRA_CFLAGS={extra}")
    subprocess.run(cmd, cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_once() -> tuple[int, bool]:
    out = subprocess.run(["./bin/esl_proxy"], cwd=ROOT, check=True, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=ENV).stdout
    d = DURATION_RE.search(out)
    t, c = TASK_RE.search(out), DONE_RE.search(out)
    ok = bool(t and c and t.group(1) == c.group(1))
    if not d:
        sys.exit("[sweep] 解析不到 duration")
    return int(d.group(1)), ok


def frame_of(fn: str) -> int:
    """从 objdump 里算出函数序言开的栈帧字节数：push 个数 x8 + sub 立即数。"""
    dis = subprocess.run(["objdump", "-d", "bin/esl_proxy", "--no-show-raw-insn"],
                         cwd=ROOT, check=True, text=True,
                         stdout=subprocess.PIPE).stdout
    body, started = [], False
    for line in dis.splitlines():
        if f"<{fn}>:" in line:
            started = True
            continue
        if started:
            if not line.strip():
                break
            body.append(line)
    total = 0
    for line in body[:30]:
        if re.search(r"\spush\s", line):
            total += 8
        m = re.search(r"\ssub\s+\$0x([0-9a-f]+),%rsp", line)
        if m:
            total += int(m.group(1), 16)
    return total


def main() -> None:
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    case, scale, runs, pad_cnt = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    configs = []
    for spec in sys.argv[5:]:
        name, _, kv = spec.partition(":")
        flags = dict(p.split("=", 1) for p in kv.split(",") if p)
        configs.append((name, flags))

    step = 4096 // pad_cnt
    pads = [PAD_BASE + step * i for i in range(pad_cnt)]
    if os.environ.get("SWEEP_REVERSE") == "1":
        pads.reverse()
    wait_quiet()
    print(f"[sweep] case={case} scale={scale} runs/变体={runs} "
          f"变体数={pad_cnt} 填充={pads[0]}..{pads[-1]} step={step}", flush=True)

    result = {}
    for name, flags in configs:
        meds, frames, bad = [], [], 0
        for pad in pads:
            build(case, scale, flags, pad)
            frames.append(frame_of("dispatch_worker"))
            run_once()  # 预热，不计入
            rows = [run_once() for _ in range(runs)]
            bad += sum(1 for _, ok in rows if not ok)
            meds.append(statistics.median(d for d, _ in rows))
        result[name] = (meds, frames, bad)
        lo, hi = min(meds), max(meds)
        print(f"  {name:<4} 变体中位数: 最小={lo:,.0f} 中位={statistics.median(meds):,.0f} "
              f"最大={hi:,.0f} ns  极差={100.0*(hi/lo-1):.1f}%  异常={bad}", flush=True)
        # 逐变体明细：用来区分「极差来自布局」还是「来自测量期间的慢漂移」。
        # 正序扫一遍再倒序扫一遍，若同样的 pad 值同样慢，就是布局；若同样的
        # 测量位次同样慢，就是漂移。
        for pad, fr, m in zip(pads, frames, meds):
            print(f"       pad={pad:<5} 帧={fr:<6} 页内偏移={(fr - 40) % 4096:<5} "
                  f"中位数={m:>13,.0f} ({m/lo:.3f}x)", flush=True)

    base_name = configs[0][0]
    base = result[base_name][0]
    print(f"\n{'配置':<6} {'变体数':>6} {'中位数的中位数':>16} {'最小':>14} {'最大':>14} "
          f"{'/'+base_name:>8} {'重叠':>6}")
    for name, _ in configs:
        meds = result[name][0]
        m = statistics.median(meds)
        # 重叠：本组有多少个变体落在基线组的 [最小, 最大] 区间内。两组分布若高度
        # 重叠，说明差异被布局抖动完全覆盖，不能声称存在稳定开销。
        overlap = sum(1 for x in meds if min(base) <= x <= max(base))
        print(f"{name:<6} {len(meds):>6} {m:>16,.0f} {min(meds):>14,.0f} "
              f"{max(meds):>14,.0f} {m/statistics.median(base):>7.4f}x "
              f"{overlap}/{len(meds):>5}")


if __name__ == "__main__":
    main()
