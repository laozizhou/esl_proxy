#!/usr/bin/env python3
"""
inject_successors.py <input.h> [-o OUTPUT]

v2: unlike the v1 tool (../gen_successors/inject_successors.py), this does
NOT split successors by which group the SOURCE task belongs to. Instead,
for every task in the whole graph, it splits that task's successors by
which group the SUCCESSOR belongs to:

    suc_cnt_N[p]  = how many of task p's successors live in group N
    successors_N  = the flat list of those group-N successor ids

`p` here ranges over ALL tasks in the whole graph (not just group N's own),
so suc_cnt_N/suc_idx_N are sized `total_task_cnt`, not that group's own
`task_cnt`. `p` is the task's position in the ascending-sorted-by-task_id
ordering of every id across all groups; this only coincides with the raw
task_id value when ids are a contiguous 0..total_task_cnt-1 range (true of
every real case file checked so far, but not assumed blindly here).

v3: also emits total_task_id/total_type/total_duration: each task's own
task_id/type/duration, merged by that same position across all groups.
Content is identical regardless of which group you'd look at it from, so
these are single global total_task_cnt-sized arrays (not per-group struct
fields), inserted once right before the `subgraph` struct typedef.

Also emits total_pre_cnt_N: same merged-by-position values as the above,
but written out once PER GROUP (own struct field, own test_graph[] entry)
instead of as one shared global - each group gets its own independent
total_task_cnt-sized copy, in case it needs to be mutated at runtime
without touching another subgraph's copy.

Same output convention as v1: writes a new file, default <input>_suc.h next
to the input; the input is never modified.
"""
import argparse
import re
import sys
from pathlib import Path

STRUCT_RE = re.compile(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*\1\s*;", re.DOTALL)
ARRAY_RE_TMPL = r"(static\s+(\S+)\s+{name}\s*\[[^\]]*\]\s*=\s*\{{)([^}}]*)(\}}\s*;)"
TOTAL_TASK_CNT_RE = re.compile(r"static\s+\S+\s+total_task_cnt\s*=\s*(\d+)\s*;")


def parse_struct_fields(text):
    m = STRUCT_RE.search(text)
    if not m:
        raise ValueError("no `typedef struct <Name> {...} <Name>;` found")
    fields = []
    for line in m.group(2).split(";"):
        line = line.strip()
        if not line:
            continue
        tokens = line.replace("*", " * ").split()
        fields.append(tokens[-1])
    return m.group(1), fields, m.span()


def find_matching_brace(text, open_pos):
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces")


def split_top_level(body):
    parts = []
    depth = 0
    current = []
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def parse_test_graph(text, struct_name):
    m = re.search(
        r"static\s+" + re.escape(struct_name) + r"\s+\w+\s*\[[^\]]*\]\s*=\s*\{",
        text,
    )
    if not m:
        raise ValueError(f"no `static {struct_name} <name>[...] = {{...}}` found")
    open_pos = m.end() - 1
    close_pos = find_matching_brace(text, open_pos)
    entries_body = text[open_pos + 1 : close_pos]

    entries = []
    depth = 0
    start = None
    for i, ch in enumerate(entries_body):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                abs_start = open_pos + 1 + start
                abs_end = open_pos + 1 + i
                entries.append((abs_start, abs_end))
    return entries


def parse_array(text, name):
    pattern = re.compile(ARRAY_RE_TMPL.format(name=re.escape(name)), re.DOTALL)
    m = pattern.search(text)
    if not m:
        raise ValueError(f"array `{name}` not found")
    elem_type = m.group(2)
    values = [int(x) for x in m.group(3).split(",") if x.strip() != ""]
    return elem_type, values, m.span()


def compute_successors(groups):
    """groups: list of dicts with task_id/pre_cnt/pre_idx/predecessors value lists.

    Returns (suc_cnt, suc_idx, successors, total_task_cnt, id_order):
    suc_cnt[g]/suc_idx[g]/successors[g] describe, for every task p in the
    whole graph (indexed by its position `pos` in id_order, ascending by
    task_id), how many/which of p's successors belong to group g.
    id_order[pos] gives the real task_id at that position.
    """
    group_of_id = {}
    for g, group in enumerate(groups):
        for tid in group["task_id"]:
            group_of_id[tid] = g

    id_order = sorted(group_of_id.keys())
    total_task_cnt = len(id_order)
    id_to_pos = {tid: pos for pos, tid in enumerate(id_order)}

    if id_order != list(range(total_task_cnt)):
        print(
            "warning: task ids are not a contiguous 0..N-1 range; "
            "suc_cnt/suc_idx are indexed by sorted position, not raw task_id",
            file=sys.stderr,
        )

    n_groups = len(groups)
    suc_cnt = [[0] * total_task_cnt for _ in range(n_groups)]
    missing = []

    for g, group in enumerate(groups):
        task_id, pre_cnt, pre_idx, predecessors = (
            group["task_id"], group["pre_cnt"], group["pre_idx"], group["predecessors"],
        )
        for i in range(len(task_id)):
            successor_id = task_id[i]
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                p_pos = id_to_pos.get(p)
                if p_pos is None:
                    missing.append((successor_id, p))
                    continue
                suc_cnt[g][p_pos] += 1

    if missing:
        print(f"warning: {len(missing)} predecessor id(s) not found in any group, dropped:", file=sys.stderr)
        for successor_id, missing_pred in missing:
            print(f"  task {successor_id} references missing predecessor {missing_pred}", file=sys.stderr)

    suc_idx = []
    successors = []
    for g in range(n_groups):
        idx = [0] * total_task_cnt
        total = 0
        for pos in range(total_task_cnt):
            idx[pos] = total
            total += suc_cnt[g][pos]
        suc_idx.append(idx)
        successors.append([0] * total)

    cursor = [idx[:] for idx in suc_idx]
    for g, group in enumerate(groups):
        task_id, pre_cnt, pre_idx, predecessors = (
            group["task_id"], group["pre_cnt"], group["pre_idx"], group["predecessors"],
        )
        for i in range(len(task_id)):
            successor_id = task_id[i]
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                p_pos = id_to_pos.get(p)
                if p_pos is None:
                    continue
                successors[g][cursor[g][p_pos]] = successor_id
                cursor[g][p_pos] += 1

    return suc_cnt, suc_idx, successors, total_task_cnt, id_order


def derive_name(predecessors_name, new_word):
    return predecessors_name.replace("predecessors", new_word)


def process_file(input_path, output_path=None):
    """Read input_path, inject suc_cnt/suc_idx/successors (target-group split,
    total_task_cnt-sized), write to output_path (default: <input>_suc.h next
    to input_path). Returns (out_path, total_local, total_edges, n_groups,
    total_task_cnt). Raises ValueError if input_path doesn't match the
    expected `subgraph` struct + `test_graph[]` shape."""
    in_path = Path(input_path)
    text = in_path.read_text()

    if output_path:
        out_path = Path(output_path)
    else:
        out_path = in_path.with_name(in_path.stem + "_suc" + in_path.suffix)

    struct_name, fields, _ = parse_struct_fields(text)
    entries = parse_test_graph(text, struct_name)

    groups = []
    group_vars = []
    for start, end in entries:
        tokens = split_top_level(text[start + 1 : end])
        var_by_field = dict(zip(fields, tokens))
        pre_cnt_type, pre_cnt_vals, _ = parse_array(text, var_by_field["pre_cnt"])
        pre_idx_type, pre_idx_vals, _ = parse_array(text, var_by_field["pre_idx"])
        pred_type, pred_vals, pred_span = parse_array(text, var_by_field["predecessors"])
        task_id_type, task_id_vals, _ = parse_array(text, var_by_field["task_id"])
        type_type, type_vals, _ = parse_array(text, var_by_field["type"])
        duration_type, duration_vals, _ = parse_array(text, var_by_field["duration"])

        if len(task_id_vals) != len(pre_cnt_vals):
            raise ValueError(
                f"`{var_by_field['task_id']}` has {len(task_id_vals)} values but "
                f"`{var_by_field['pre_cnt']}` has {len(pre_cnt_vals)} - they must match"
            )
        if len(task_id_vals) != len(type_vals):
            raise ValueError(
                f"`{var_by_field['task_id']}` has {len(task_id_vals)} values but "
                f"`{var_by_field['type']}` has {len(type_vals)} - they must match"
            )
        if len(task_id_vals) != len(duration_vals):
            raise ValueError(
                f"`{var_by_field['task_id']}` has {len(task_id_vals)} values but "
                f"`{var_by_field['duration']}` has {len(duration_vals)} - they must match"
            )

        groups.append({
            "task_id": task_id_vals,
            "pre_cnt": pre_cnt_vals,
            "pre_idx": pre_idx_vals,
            "predecessors": pred_vals,
            "type": type_vals,
            "duration": duration_vals,
        })
        group_vars.append({
            "predecessors_var": var_by_field["predecessors"],
            "predecessors_span": pred_span,
            "pre_cnt_type": pre_cnt_type,
            "pre_idx_type": pre_idx_type,
            "pred_type": pred_type,
            "task_id_type": task_id_type,
            "type_type": type_type,
            "duration_type": duration_type,
        })

    suc_cnt, suc_idx, successors, total_task_cnt, id_order = compute_successors(groups)

    id_to_pos = {tid: pos for pos, tid in enumerate(id_order)}

    # total_task_id/total_type/total_duration/total_pre_cnt: every task's own
    # value, merged by position across all groups.
    total_task_id_vals = list(id_order)
    total_type_vals = [None] * total_task_cnt
    total_duration_vals = [None] * total_task_cnt
    total_pre_cnt_vals = [None] * total_task_cnt
    for group in groups:
        for i, tid in enumerate(group["task_id"]):
            pos = id_to_pos[tid]
            total_type_vals[pos] = group["type"][i]
            total_duration_vals[pos] = group["duration"][i]
            total_pre_cnt_vals[pos] = group["pre_cnt"][i]

    declared_m = TOTAL_TASK_CNT_RE.search(text)
    if declared_m and int(declared_m.group(1)) != total_task_cnt:
        print(
            f"warning: file declares total_task_cnt={declared_m.group(1)} but "
            f"sum of all groups' task_id[] lengths is {total_task_cnt}",
            file=sys.stderr,
        )

    edits = []

    for g, gv in enumerate(group_vars):
        suc_cnt_name = derive_name(gv["predecessors_var"], "suc_cnt")
        suc_idx_name = derive_name(gv["predecessors_var"], "suc_idx")
        successors_name = derive_name(gv["predecessors_var"], "successors")
        total_pre_cnt_name = derive_name(gv["predecessors_var"], "total_pre_cnt")
        block = (
            f"\nstatic {gv['pre_cnt_type']} {suc_cnt_name}[{total_task_cnt}] = "
            f"{{{', '.join(str(x) for x in suc_cnt[g])}}};"
            f"\nstatic {gv['pre_idx_type']} {suc_idx_name}[{total_task_cnt}] = "
            f"{{{', '.join(str(x) for x in suc_idx[g])}}};"
            f"\nstatic {gv['pred_type']} {successors_name}[] = "
            f"{{{', '.join(str(x) for x in successors[g])}}};"
            f"\nstatic {gv['pre_cnt_type']} {total_pre_cnt_name}[{total_task_cnt}] = "
            f"{{{', '.join(str(x) for x in total_pre_cnt_vals)}}};"
        )
        insert_pos = gv["predecessors_span"][1]
        edits.append((insert_pos, block))
        gv["suc_cnt_name"] = suc_cnt_name
        gv["suc_idx_name"] = suc_idx_name
        gv["successors_name"] = successors_name
        gv["total_pre_cnt_name"] = total_pre_cnt_name

    struct_name2, fields2, struct_span = parse_struct_fields(text)
    struct_close = text.rfind("}", struct_span[0], struct_span[1])
    first = group_vars[0]
    struct_field_block = (
        f"    {first['pre_cnt_type']}* suc_cnt;\n"
        f"    {first['pre_idx_type']}* suc_idx;\n"
        f"    {first['pred_type']}* successors;\n"
        f"    {first['pre_cnt_type']}* total_pre_cnt;\n"
    )
    edits.append((struct_close, struct_field_block))

    # total_task_id/total_type/total_duration: every task's own value,
    # identical regardless of which group you'd look at it from, so each is
    # a single global array (not a per-group struct field). Placed right
    # next to `total_task_cnt` (no blank line - same "global summary" family
    # of declarations); falls back to right before the struct typedef if the
    # file has no `total_task_cnt` line to anchor on. total_pre_cnt is
    # different - it's per-group (see above) since it may need independent
    # per-subgraph copies at runtime, so it's NOT included here.
    global_arrays_block = (
        f"\nstatic {first['task_id_type']} total_task_id[{total_task_cnt}] = "
        f"{{{', '.join(str(x) for x in total_task_id_vals)}}};"
        f"\nstatic {first['type_type']} total_type[{total_task_cnt}] = "
        f"{{{', '.join(str(x) for x in total_type_vals)}}};"
        f"\nstatic {first['duration_type']} total_duration[{total_task_cnt}] = "
        f"{{{', '.join(str(x) for x in total_duration_vals)}}};"
    )
    if declared_m:
        edits.append((declared_m.end(), global_arrays_block))
    else:
        edits.append((struct_span[0], global_arrays_block + "\n\n"))

    for (start, end), gv in zip(entries, group_vars):
        addition = (
            f", {gv['suc_cnt_name']}, {gv['suc_idx_name']}, "
            f"{gv['successors_name']}, {gv['total_pre_cnt_name']}"
        )
        edits.append((end, addition))

    edits.sort(key=lambda e: e[0], reverse=True)
    result = text
    for pos, insertion in edits:
        result = result[:pos] + insertion + result[pos:]

    out_path.write_text(result)

    total_edges = sum(len(g["predecessors"]) for g in groups)
    total_local = sum(len(s) for s in successors)
    return out_path, total_local, total_edges, len(groups), total_task_cnt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="path to the original .h file (read-only)")
    ap.add_argument("-o", "--output", help="output path (default: <input>_suc.h next to the input)")
    args = ap.parse_args()

    out_path, total_local, total_edges, n_groups, total_task_cnt = process_file(args.input, args.output)
    print(
        f"wrote {out_path}: {total_local}/{total_edges} edges resolved across "
        f"{n_groups} group(s), total_task_cnt={total_task_cnt}"
    )


if __name__ == "__main__":
    main()
