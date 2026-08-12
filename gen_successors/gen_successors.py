import re
import sys

SRC = "predecessors.h"
DST = "successors.h"

ARRAY_RE = re.compile(r"static\s+\S+\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}\s*;", re.DOTALL)


def parse_arrays(text):
    arrays = {}
    for name, body in ARRAY_RE.findall(text):
        arrays[name] = [int(x) for x in body.split(",") if x.strip() != ""]
    return arrays


def build_id_lookup(groups):
    """id -> (group_index, local_index) across all groups."""
    lookup = {}
    for g, group in enumerate(groups):
        for i, task_id in enumerate(group["task_id"]):
            lookup[task_id] = (g, i)
    return lookup


def compute_successors(groups):
    lookup = build_id_lookup(groups)
    suc_cnt = [[0] * len(g["task_id"]) for g in groups]
    missing = []

    for group in groups:
        task_id, pre_cnt, pre_idx, predecessors = (
            group["task_id"], group["pre_cnt"], group["pre_idx"], group["predecessors"],
        )
        for i in range(len(task_id)):
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                target = lookup.get(p)
                if target is None:
                    missing.append((task_id[i], p))
                    continue
                tg, ti = target
                suc_cnt[tg][ti] += 1

    if missing:
        print(f"warning: {len(missing)} predecessor id(s) not found in any group, dropped:", file=sys.stderr)
        for task_id, missing_pred in missing:
            print(f"  task {task_id} references missing predecessor {missing_pred}", file=sys.stderr)

    suc_idx = []
    successors = []
    for g, group in enumerate(groups):
        idx = [0] * len(suc_cnt[g])
        total = 0
        for i in range(len(suc_cnt[g])):
            idx[i] = total
            total += suc_cnt[g][i]
        arr = [0] * total
        cursor = idx[:]
        suc_idx.append(idx)
        successors.append(arr)

    cursor = [idx[:] for idx in suc_idx]  # per (group, local index) next-write position
    for group in groups:
        task_id, pre_cnt, pre_idx, predecessors = (
            group["task_id"], group["pre_cnt"], group["pre_idx"], group["predecessors"],
        )
        for i in range(len(task_id)):
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                target = lookup.get(p)
                if target is None:
                    continue
                tg, ti = target
                successors[tg][cursor[tg][ti]] = task_id[i]
                cursor[tg][ti] += 1

    return suc_cnt, suc_idx, successors


def fmt_fixed(name, typ, arr):
    return f"static {typ} {name}[{len(arr)}] = {{{', '.join(str(x) for x in arr)}}};"


def fmt_flex(name, typ, arr):
    return f"static {typ} {name}[] = {{{', '.join(str(x) for x in arr)}}};"


def main():
    text = open(SRC, "r").read()
    arrays = parse_arrays(text)

    groups = [
        {
            "task_id": arrays["task_id_1"],
            "pre_cnt": arrays["pre_cnt_1"],
            "pre_idx": arrays["pre_idx_1"],
            "predecessors": arrays["predecessors_1"],
        },
        {
            "task_id": arrays["task_id_2"],
            "pre_cnt": arrays["pre_cnt_2"],
            "pre_idx": arrays["pre_idx_2"],
            "predecessors": arrays["predecessors_2"],
        },
    ]

    suc_cnt, suc_idx, successors = compute_successors(groups)

    lines = []
    for g, group in enumerate(groups, start=1):
        lines.append(fmt_fixed(f"task_id_{g}", "uint32_t", group["task_id"]))
        lines.append(fmt_fixed(f"suc_cnt_{g}", "int", suc_cnt[g - 1]))
        lines.append(fmt_fixed(f"suc_idx_{g}", "int", suc_idx[g - 1]))
        lines.append(fmt_flex(f"successors_{g}", "uint32_t", successors[g - 1]))
        lines.append("")

    with open(DST, "w") as f:
        f.write("\n".join(lines).rstrip() + "\n")

    total_edges = sum(len(g["predecessors"]) for g in groups)
    total_local = sum(len(s) for s in successors)
    print(f"wrote {DST}: {total_local}/{total_edges} edges resolved locally within the two groups")


if __name__ == "__main__":
    main()
