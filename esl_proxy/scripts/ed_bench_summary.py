#!/usr/bin/env python3
import csv
import glob
import os
import re
import statistics
import subprocess
import sys
from collections import Counter, defaultdict


def extract_int(pattern: str, text: str):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


def extract_float(pattern: str, text: str):
    m = re.search(pattern, text)
    return float(m.group(1)) if m else None


def median_or_none(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return statistics.median(values)


def fmt_num(value, digits=6):
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def parse_record(path: str):
    text = open(path, "r", encoding="utf-8", errors="ignore").read()
    m = re.match(r"(.+)_ed([01])_r(\d+)\.log$", os.path.basename(path))
    if not m:
        return None

    case = m.group(1) + ".h"
    ed_enable = int(m.group(2))
    rep = int(m.group(3))

    completed = extract_int(r"\[scheduler\] task_cnt = (\d+)", text)
    if completed is None:
        completed = extract_int(r"\[summary\] completed_task_cnt = (\d+)", text)
    if completed is None:
        completed = extract_int(r"completed_task_cnt,(\d+)", text)

    notify_counter = Counter(re.findall(r"notify_write, s=(\d+)", text))
    # harness 写入的开关标记；缺失时按开启处理（兼容旧日志）
    worker_log = extract_int(r"\[bench\] worker_log=(\d+)", text)
    worker_log = 1 if worker_log is None else worker_log

    return {
        "path": path,
        "case": case,
        "ed_enable": ed_enable,
        "rep": rep,
        "task_cnt": extract_int(r"\[orchestration\] task_cnt = (\d+)", text),
        "completed_task_cnt": completed,
        "orch_elapsed_ns": extract_int(r"\[orchestration\] elapsed_time = (\d+) ns", text),
        "sched_elapsed_ns": extract_int(r"\[scheduler\] duration = (\d+) ns", text),
        "sched_task_tp": extract_float(r"\[scheduler\] task_tp = ([0-9.]+) MTasks/s", text),
        "stage_cnt": extract_int(r"\[ed\] stage_cnt = (\d+)", text),
        "hit_cnt": extract_int(r"\[ed\] hit_cnt = (\d+)", text),
        "self_notify_cnt": extract_int(r"\[ed\] self_notify_cnt = (\d+)", text),
        "slot_retry_cnt": extract_int(r"\[ed\] slot_retry_cnt = (\d+)", text),
        "block_cas_fail_cnt": extract_int(r"\[ed\] block_cas_fail_cnt = (\d+)", text),
        "send_skip_cnt": extract_int(r"\[ed\] send_skip_cnt = (\d+)", text),
        "leaked_staging": extract_int(r"\[ed\] leaked_staging = (\d+)", text),
        "block_leaked": extract_int(r"\[ed\] block_leaked = (\d+)", text),
        "slot_leaked": extract_int(r"\[ed\] slot_leaked = (\d+)", text),
        "gate_open_cnt": extract_int(r"\[ed\] gate_open_cnt = (\d+)", text),
        "notify_counter": notify_counter,
        "worker_log": worker_log,
        # ready->runnable 延迟：ED 的直接 KPI，normal/ed 两条路径分开
        "lat_normal_cnt": extract_int(r"\[lat\] normal: samples = (\d+)", text),
        "lat_normal_mean_ns": extract_float(
            r"\[lat\] normal: samples = \d+, mean = ([0-9.]+) ns", text
        ),
        "lat_normal_p50_ns": extract_int(r"\[lat\] normal: p50 <= (\d+) ns", text),
        "lat_ed_cnt": extract_int(r"\[lat\] ed: samples = (\d+)", text),
        "lat_ed_mean_ns": extract_float(
            r"\[lat\] ed: samples = \d+, mean = ([0-9.]+) ns", text
        ),
        "lat_ed_p50_ns": extract_int(r"\[lat\] ed: p50 <= (\d+) ns", text),
    }


def check_assertions(records, repo_root):
    errors = []

    for r in records:
        # A1
        if r["task_cnt"] is None or r["completed_task_cnt"] is None:
            errors.append(f"A1 missing metric: {r['path']}")
        elif r["task_cnt"] != r["completed_task_cnt"]:
            errors.append(
                f"A1 fail {r['path']}: completed={r['completed_task_cnt']} task={r['task_cnt']}"
            )

        if r["ed_enable"] != 1:
            continue

        # A2
        required = ["stage_cnt", "hit_cnt", "self_notify_cnt"]
        if any(r[k] is None for k in required):
            errors.append(f"A2 missing ed metric: {r['path']}")
        elif r["hit_cnt"] + r["self_notify_cnt"] != r["stage_cnt"]:
            errors.append(
                f"A2 fail {r['path']}: hit+self={r['hit_cnt'] + r['self_notify_cnt']} stage={r['stage_cnt']}"
            )

        # A3：依赖逐条 notify_write 日志，WORKER_LOG=0 的运行无法判定，跳过
        if r["worker_log"] == 0:
            pass
        elif r["stage_cnt"] and r["stage_cnt"] > 0:
            total_notify = sum(r["notify_counter"].values())
            if total_notify == 0:
                errors.append(f"A3 missing notify_write logs: {r['path']} (WORKER_LOG=1 required)")
            else:
                bad = [(task, n) for task, n in r["notify_counter"].items() if n != 1]
                if bad:
                    errors.append(f"A3 fail {r['path']}: notify count != 1 for {bad[:5]}")
                if total_notify != r["stage_cnt"]:
                    errors.append(
                        f"A3 fail {r['path']}: total_notify={total_notify} stage={r['stage_cnt']}"
                    )

        # A5
        if r["leaked_staging"] is None:
            errors.append(f"A5 missing leaked_staging: {r['path']}")
        elif r["leaked_staging"] != 0:
            errors.append(f"A5 fail {r['path']}: leaked_staging={r['leaked_staging']}")

        # A6
        if r["block_leaked"] is None:
            errors.append(f"A6 missing block_leaked: {r['path']}")
        elif r["block_leaked"] != 0:
            errors.append(f"A6 fail {r['path']}: block_leaked={r['block_leaked']}")

        # A7
        needed_a7 = ["send_skip_cnt", "stage_cnt", "block_cas_fail_cnt"]
        if any(r[k] is None for k in needed_a7):
            errors.append(f"A7 missing metric: {r['path']}")
        else:
            diff = abs(r["send_skip_cnt"] - r["stage_cnt"])
            if diff > r["block_cas_fail_cnt"]:
                errors.append(
                    f"A7 fail {r['path']}: |skip-stage|={diff} > block_cas_fail={r['block_cas_fail_cnt']}"
                )

        # A8
        if r["slot_leaked"] is None:
            errors.append(f"A8 missing slot_leaked: {r['path']}")
        elif r["slot_leaked"] != 0:
            errors.append(f"A8 fail {r['path']}: slot_leaked={r['slot_leaked']}")

        # 门铃协议对账：notify 只敲门铃，实际开闸由 executor 完成，
        # 每次成功通知必须恰好换来一次开闸，否则说明有通知被丢或被重复消费。
        if r["gate_open_cnt"] is None:
            errors.append(f"GATE missing gate_open_cnt: {r['path']}")
        elif None not in (r["hit_cnt"], r["self_notify_cnt"]):
            notified = r["hit_cnt"] + r["self_notify_cnt"]
            if r["gate_open_cnt"] != notified:
                errors.append(
                    f"GATE fail {r['path']}: gate_open={r['gate_open_cnt']} "
                    f"hit+self={notified}"
                )

        # KPI 自洽：每个被 staged 且成功放行的任务贡献恰好一个 ED 路径样本，
        # 所以样本数不应超过 stage_cnt；stage 了却收不到样本说明打点漏了。
        if r["lat_ed_cnt"] is None:
            errors.append(f"LAT missing ed samples: {r['path']}")
        elif r["stage_cnt"] is not None:
            if r["lat_ed_cnt"] > r["stage_cnt"]:
                errors.append(
                    f"LAT fail {r['path']}: ed samples={r['lat_ed_cnt']} > stage={r['stage_cnt']}"
                )
            elif r["stage_cnt"] > 0 and r["lat_ed_cnt"] == 0:
                errors.append(
                    f"LAT fail {r['path']}: stage={r['stage_cnt']} but ed samples=0"
                )

    # A4: per case same task count under ED=0/1
    by_case = defaultdict(lambda: {0: set(), 1: set()})
    for r in records:
        if r["task_cnt"] is not None:
            by_case[r["case"]][r["ed_enable"]].add(r["task_cnt"])
    for case, pair in by_case.items():
        if len(pair[0]) != 1 or len(pair[1]) != 1:
            errors.append(
                f"A4 fail {case}: ed0 task_cnt={sorted(pair[0])}, ed1 task_cnt={sorted(pair[1])}"
            )
            continue
        if next(iter(pair[0])) != next(iter(pair[1])):
            errors.append(
                f"A4 fail {case}: ed0={next(iter(pair[0]))}, ed1={next(iter(pair[1]))}"
            )

    # A9: bitmap snapshot protocol regression
    step2 = subprocess.run(
        ["make", "-s", "test-step2"],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if step2.returncode != 0:
        errors.append(f"A9 fail: make test-step2 rc={step2.returncode}")

    return errors


def aggregate_rows(records):
    grouped = defaultdict(list)
    for r in records:
        grouped[(r["case"], r["ed_enable"])].append(r)

    med = {}
    for key, runs in grouped.items():
        med[key] = {
            "elapsed_ns_median": median_or_none([r["sched_elapsed_ns"] for r in runs]),
            "task_tp_median": median_or_none([r["sched_task_tp"] for r in runs]),
            "stage_cnt_median": median_or_none([r["stage_cnt"] for r in runs]),
            "hit_cnt_median": median_or_none([r["hit_cnt"] for r in runs]),
            "self_notify_cnt_median": median_or_none([r["self_notify_cnt"] for r in runs]),
            "slot_retry_cnt_median": median_or_none([r["slot_retry_cnt"] for r in runs]),
            "send_skip_cnt_median": median_or_none([r["send_skip_cnt"] for r in runs]),
            "lat_normal_mean_median": median_or_none([r["lat_normal_mean_ns"] for r in runs]),
            "lat_normal_p50_median": median_or_none([r["lat_normal_p50_ns"] for r in runs]),
            "lat_ed_mean_median": median_or_none([r["lat_ed_mean_ns"] for r in runs]),
            "lat_ed_p50_median": median_or_none([r["lat_ed_p50_ns"] for r in runs]),
            "lat_ed_cnt_median": median_or_none([r["lat_ed_cnt"] for r in runs]),
        }

    rows = []
    for case, ed in sorted(grouped.keys()):
        m = med[(case, ed)]
        stage = m["stage_cnt_median"]
        hit = m["hit_cnt_median"]
        selfn = m["self_notify_cnt_median"]
        doorbell_ratio = None
        self_notify_ratio = None
        if stage and stage > 0 and hit is not None and selfn is not None:
            doorbell_ratio = (hit + selfn) / stage
            if (hit + selfn) > 0:
                self_notify_ratio = selfn / (hit + selfn)

        speedup = None
        throughput_gain = None
        lat_x_baseline = None
        if ed == 1 and (case, 0) in med:
            base_elapsed = med[(case, 0)]["elapsed_ns_median"]
            ed_elapsed = m["elapsed_ns_median"]
            base_tp = med[(case, 0)]["task_tp_median"]
            ed_tp = m["task_tp_median"]
            if base_elapsed and ed_elapsed:
                speedup = base_elapsed / ed_elapsed
            if base_tp and ed_tp:
                throughput_gain = (ed_tp - base_tp) / base_tp
            # ED 放行路径的延迟相对 ED=0 构建的正常路径延迟降低了多少倍
            base_lat = med[(case, 0)]["lat_normal_mean_median"]
            ed_lat = m["lat_ed_mean_median"]
            if base_lat and ed_lat:
                lat_x_baseline = base_lat / ed_lat

        # 同一次运行内 ED 路径相对正常路径的延迟倍数
        lat_x_inrun = None
        if m["lat_normal_mean_median"] and m["lat_ed_mean_median"]:
            lat_x_inrun = m["lat_normal_mean_median"] / m["lat_ed_mean_median"]

        rows.append(
            {
                "case": case,
                "ed_enable": ed,
                "elapsed_ns_median": m["elapsed_ns_median"],
                "task_tp_median": m["task_tp_median"],
                "stage_cnt": m["stage_cnt_median"],
                "hit_cnt": m["hit_cnt_median"],
                "self_notify_cnt": m["self_notify_cnt_median"],
                "doorbell_ratio": doorbell_ratio,
                "self_notify_ratio": self_notify_ratio,
                "slot_retry_cnt": m["slot_retry_cnt_median"],
                "send_skip_cnt": m["send_skip_cnt_median"],
                "speedup": speedup,
                "throughput_gain": throughput_gain,
                "lat_normal_mean_ns": m["lat_normal_mean_median"],
                "lat_normal_p50_ns": m["lat_normal_p50_median"],
                "lat_ed_mean_ns": m["lat_ed_mean_median"],
                "lat_ed_p50_ns": m["lat_ed_p50_median"],
                "lat_ed_samples": m["lat_ed_cnt_median"],
                "lat_x_inrun": lat_x_inrun,
                "lat_x_baseline": lat_x_baseline,
            }
        )
    return rows


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {os.path.basename(sys.argv[0])} <log_dir>", file=sys.stderr)
        sys.exit(2)

    log_dir = sys.argv[1]
    files = sorted(glob.glob(os.path.join(log_dir, "*.log")))
    if not files:
        print(f"No logs found under: {log_dir}", file=sys.stderr)
        sys.exit(2)

    records = []
    for path in files:
        rec = parse_record(path)
        if rec is not None:
            records.append(rec)

    if not records:
        print(f"No valid benchmark logs under: {log_dir}", file=sys.stderr)
        sys.exit(2)

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    errors = check_assertions(records, repo_root)
    rows = aggregate_rows(records)

    fieldnames = [
        "case",
        "ed_enable",
        "elapsed_ns_median",
        "task_tp_median",
        "stage_cnt",
        "hit_cnt",
        "self_notify_cnt",
        "doorbell_ratio",
        "self_notify_ratio",
        "slot_retry_cnt",
        "send_skip_cnt",
        "speedup",
        "throughput_gain",
        "lat_normal_mean_ns",
        "lat_normal_p50_ns",
        "lat_ed_mean_ns",
        "lat_ed_p50_ns",
        "lat_ed_samples",
        "lat_x_inrun",
        "lat_x_baseline",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    for row in rows:
        writer.writerow({k: fmt_num(v) for k, v in row.items()})

    if errors:
        for err in errors:
            print(err, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
