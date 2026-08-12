#!/usr/bin/env python3
"""
inject_successors.py <input.h> [-o OUTPUT]

Reads a scheduler case header (a `subgraph` struct + `test_graph[]` array
of one-or-more groups, each holding task_id/pre_cnt/pre_idx/predecessors
arrays), computes every task's successors from the predecessor data (CSR
transpose, combining all groups so cross-group edges resolve), and writes
a new header where:
  - each group gets new suc_cnt/suc_idx/successors arrays right after its
    predecessors array,
  - the subgraph struct gets three new pointer fields,
  - each test_graph[] entry gets three new pointer arguments.

The input file is never modified; the result is written to a new file
(default: same directory, "_suc" appended before the extension).
"""
import argparse
import re
import sys
from pathlib import Path

STRUCT_RE = re.compile(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*\1\s*;", re.DOTALL)
ARRAY_RE_TMPL = r"(static\s+(\S+)\s+{name}\s*\[[^\]]*\]\s*=\s*\{{)([^}}]*)(\}}\s*;)"


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
    """open_pos indexes the '{' character; return index of its matching '}'."""
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
    """Split a comma-separated list, only at depth 0 (ignoring commas inside {})."""
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
    Returns parallel list of (suc_cnt, suc_idx, successors) per group."""
    lookup = {}
    for g, group in enumerate(groups):
        for i, tid in enumerate(group["task_id"]):
            lookup[tid] = (g, i)

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
        for tid, missing_pred in missing:
            print(f"  task {tid} references missing predecessor {missing_pred}", file=sys.stderr)

    suc_idx = []
    successors = [[] for _ in groups]
    for g in range(len(groups)):
        idx = [0] * len(suc_cnt[g])
        total = 0
        for i in range(len(suc_cnt[g])):
            idx[i] = total
            total += suc_cnt[g][i]
        suc_idx.append(idx)
        successors[g] = [0] * total

    cursor = [idx[:] for idx in suc_idx]
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


def derive_name(predecessors_name, new_word):
    return predecessors_name.replace("predecessors", new_word)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="path to the original .h file (read-only)")
    ap.add_argument("-o", "--output", help="output path (default: <input>_suc.h next to the input)")
    args = ap.parse_args()

    in_path = Path(args.input)
    text = in_path.read_text()

    if args.output:
        out_path = Path(args.output)
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
        _, task_id_vals, _ = parse_array(text, var_by_field["task_id"])
        groups.append({
            "task_id": task_id_vals,
            "pre_cnt": pre_cnt_vals,
            "pre_idx": pre_idx_vals,
            "predecessors": pred_vals,
        })
        group_vars.append({
            "predecessors_var": var_by_field["predecessors"],
            "predecessors_span": pred_span,
            "pre_cnt_type": pre_cnt_type,
            "pre_idx_type": pre_idx_type,
            "pred_type": pred_type,
        })

    suc_cnt, suc_idx, successors = compute_successors(groups)

    # Collect (position, text_to_insert) edits, applied back-to-front so earlier
    # offsets stay valid.
    edits = []

    for g, gv in enumerate(group_vars):
        suc_cnt_name = derive_name(gv["predecessors_var"], "suc_cnt")
        suc_idx_name = derive_name(gv["predecessors_var"], "suc_idx")
        successors_name = derive_name(gv["predecessors_var"], "successors")
        n = len(suc_cnt[g])
        block = (
            f"\nstatic {gv['pre_cnt_type']} {suc_cnt_name}[{n}] = "
            f"{{{', '.join(str(x) for x in suc_cnt[g])}}};"
            f"\nstatic {gv['pre_idx_type']} {suc_idx_name}[{n}] = "
            f"{{{', '.join(str(x) for x in suc_idx[g])}}};"
            f"\nstatic {gv['pred_type']} {successors_name}[] = "
            f"{{{', '.join(str(x) for x in successors[g])}}};"
        )
        insert_pos = gv["predecessors_span"][1]  # right after the `};`
        edits.append((insert_pos, block))
        gv["suc_cnt_name"] = suc_cnt_name
        gv["suc_idx_name"] = suc_idx_name
        gv["successors_name"] = successors_name

    # struct fields: insert right before the closing brace of the typedef.
    struct_name2, fields2, struct_span = parse_struct_fields(text)
    struct_close = text.rfind("}", struct_span[0], struct_span[1])
    first = group_vars[0]
    struct_field_block = (
        f"    {first['pre_cnt_type']}* suc_cnt;\n"
        f"    {first['pre_idx_type']}* suc_idx;\n"
        f"    {first['pred_type']}* successors;\n"
    )
    edits.append((struct_close, struct_field_block))

    # test_graph[] entries: append the three new pointer args before each entry's `}`.
    for (start, end), gv in zip(entries, group_vars):
        addition = f", {gv['suc_cnt_name']}, {gv['suc_idx_name']}, {gv['successors_name']}"
        edits.append((end, addition))

    edits.sort(key=lambda e: e[0], reverse=True)
    result = text
    for pos, insertion in edits:
        result = result[:pos] + insertion + result[pos:]

    out_path.write_text(result)

    total_edges = sum(len(g["predecessors"]) for g in groups)
    total_local = sum(len(s) for s in successors)
    print(f"wrote {out_path}: {total_local}/{total_edges} edges resolved across {len(groups)} group(s)")


if __name__ == "__main__":
    main()
