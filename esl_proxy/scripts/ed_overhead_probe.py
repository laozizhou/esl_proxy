#!/usr/bin/env python3
"""
ED 常驻开销探针：只比 makespan，用于验证「某一处热路径改动值多少」。

用法:
  ed_overhead_probe.py <label> <case> <scale> <repeat> <variant> [<variant> ...]
  variant 形如  B:ED_ENABLE=1,ED_UNFIN_THRESHOLD=0

与 ed_server_perf.py 的区别：默认 LAT_TRACE=0，去掉打点对 makespan 的干扰；
每个 variant 连续跑 repeat 次，输出中位数 / CV / 相对第一个 variant 的倍数。
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

# 单次运行只有几十毫秒，机器上有别人的编译任务就会把 CV 从 0.05 拉到 0.9。
# 实测踩过一次：load 27 时同一配置的中位数漂了 2 倍。所以开跑前必须等机器安静。
MAX_LOAD1 = float(os.environ.get("PROBE_MAX_LOAD", "3.0"))
WAIT_LOAD_SEC = int(os.environ.get("PROBE_WAIT_LOAD_SEC", "0"))


def load1() -> float:
    return os.getloadavg()[0]


def wait_for_quiet_machine() -> None:
    if load1() <= MAX_LOAD1:
        return
    if WAIT_LOAD_SEC <= 0:
        sys.exit(f"[probe] 中止：load1={load1():.1f} > {MAX_LOAD1}，机器不干净。"
                 f" 设 PROBE_WAIT_LOAD_SEC=<秒> 可改为等待。")
    deadline = time.time() + WAIT_LOAD_SEC
    while time.time() < deadline:
        cur = load1()
        print(f"[probe] 等待机器空闲：load1={cur:.1f} > {MAX_LOAD1}，"
              f"剩余 {int(deadline - time.time())}s", flush=True)
        if cur <= MAX_LOAD1:
            # 负载刚落下来，多等一会儿让缓存/频率稳定
            time.sleep(10)
            if load1() <= MAX_LOAD1:
                return
        time.sleep(20)
    sys.exit(f"[probe] 中止：等了 {WAIT_LOAD_SEC}s，load1 仍为 {load1():.1f}")

PATTERNS = {
    "task_cnt": r"\[orchestration\] task_cnt = (\d+)",
    "completed": r"\[summary\] completed_task_cnt = (\d+)",
    "duration_ns": r"\[scheduler\] duration = (\d+) ns",
    "stage_cnt": r"\[ed\] stage_cnt = (\d+)",
    "gate_open_cnt": r"\[ed\] gate_open_cnt = (\d+)",
    "leaked_staging": r"\[ed\] leaked_staging = (\d+)",
    "slot_leaked": r"\[ed\] slot_leaked = (\d+)",
    "dispatch_rounds": r"\[trace\] dispatch_rounds = (\d+)",
}


def parse(text: str) -> dict:
    row = {}
    for k, pat in PATTERNS.items():
        m = re.search(pat, text)
        row[k] = int(m.group(1)) if m else None
    return row


def build(case: str, scale: str, flags: dict) -> None:
    subprocess.run(["make", "-s", "clean"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cmd = ["make", "-s", "all", f"CASE={case}",
           f"EXEC_DURATION_SCALE={scale}", "LAT_TRACE=0"]
    cmd += [f"{k}={v}" for k, v in flags.items()]
    subprocess.run(cmd, cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_once() -> dict:
    out = subprocess.run(["./bin/esl_proxy"], cwd=ROOT, check=True, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env={"WORKER_LOG": "0", "PATH": "/usr/bin:/bin"})
    return parse(out.stdout)


def main():
    label, case, scale, repeat = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    variants = []
    for spec in sys.argv[5:]:
        name, _, kvs = spec.partition(":")
        flags = dict(kv.split("=", 1) for kv in kvs.split(",") if kv)
        variants.append((name, flags))

    wait_for_quiet_machine()
    print(f"[probe] {label} case={case} scale={scale} repeat={repeat} "
          f"load1={load1():.2f}", flush=True)
    results = {}
    for name, flags in variants:
        build(case, scale, flags)
        # 编译本身会推高 load，等它落回来再测，否则第一个 variant 被系统性拖慢
        while load1() > MAX_LOAD1:
            time.sleep(5)
        load_before = load1()
        rows = [run_once() for _ in range(repeat)]
        load_after = load1()
        durs = [r["duration_ns"] for r in rows]
        ok = all(r["completed"] == r["task_cnt"] for r in rows)
        if not ok:
            # 多线程同时写 stdout 会撕裂日志行，导致正则抓不到字段。
            # 打出原始行区分「真跑挂了」和「只是没解析到」。
            for i, r in enumerate(rows):
                if r["completed"] != r["task_cnt"]:
                    print(f"  !! {name} rep{i+1}: task_cnt={r['task_cnt']} "
                          f"completed={r['completed']} dur={r['duration_ns']}", flush=True)
        stage = statistics.median(r["stage_cnt"] or 0 for r in rows)
        gate = statistics.median(r["gate_open_cnt"] or 0 for r in rows)
        leak = max((r["leaked_staging"] or 0) + (r["slot_leaked"] or 0) for r in rows)
        results[name] = {
            "med": statistics.median(durs),
            "min": min(durs),
            "cv": statistics.stdev(durs) / statistics.mean(durs) if len(durs) > 1 else 0.0,
            "rounds": statistics.median(r["dispatch_rounds"] or 0 for r in rows),
            "stage": stage, "gate": gate, "leak": leak, "ok": ok,
            "load": max(load_before, load_after),
        }
        print(f"  {name:<10} flags={flags}", flush=True)

    base = results[variants[0][0]]
    print(f"\n{'variant':<12} {'med_ns':>12} {'min_ns':>12} {'CV':>7} "
          f"{'/base':>7} {'min/base':>9} {'load':>6} {'stage':>7} {'gate':>7} {'ok':>4}")
    for name, _ in variants:
        r = results[name]
        print(f"{name:<12} {r['med']:>12,.0f} {r['min']:>12,.0f} {r['cv']:>7.3f} "
              f"{r['med']/base['med']:>6.3f}x {r['min']/base['min']:>8.3f}x "
              f"{r['load']:>6.2f} {r['stage']:>7,.0f} {r['gate']:>7,.0f} "
              f"{('OK' if r['ok'] and r['leak'] == 0 else 'FAIL'):>4}")
        if r["cv"] > 0.20:
            print(f"  !! {name} 的 CV={r['cv']:.2f} 过大，该行不可用（机器不干净？）",
                  flush=True)


if __name__ == "__main__":
    main()
