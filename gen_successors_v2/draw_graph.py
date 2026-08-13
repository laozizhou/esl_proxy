#!/usr/bin/env python3
"""
draw_graph.py <generated_suc.h> [-o output.md]

Builds two edge lists for the same DAG, both read from the SAME generated
_suc.h file (it still has the original predecessors/pre_cnt/pre_idx/task_id
data - inject_successors.py only appends, never removes): one from the
predecessors data, one from the generated successors data. Emits both as
mermaid flowchart blocks in a single markdown file, so they can be visually
compared side by side.
"""
import argparse
import sys
from pathlib import Path

from inject_successors import parse_struct_fields, parse_test_graph, split_top_level, parse_array


def edges_from_predecessors(text):
    struct_name, fields, _ = parse_struct_fields(text)
    entries = parse_test_graph(text, struct_name)
    edges = []
    for start, end in entries:
        tokens = split_top_level(text[start + 1 : end])
        var_by_field = dict(zip(fields, tokens))
        _, task_id, _ = parse_array(text, var_by_field["task_id"])
        _, pre_cnt, _ = parse_array(text, var_by_field["pre_cnt"])
        _, pre_idx, _ = parse_array(text, var_by_field["pre_idx"])
        _, predecessors, _ = parse_array(text, var_by_field["predecessors"])
        for i in range(len(task_id)):
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                edges.append((p, task_id[i]))
    return edges


def edges_from_successors(text):
    struct_name, fields, _ = parse_struct_fields(text)
    entries = parse_test_graph(text, struct_name)

    group_task_ids = []
    group_suc = []
    for start, end in entries:
        tokens = split_top_level(text[start + 1 : end])
        var_by_field = dict(zip(fields, tokens))
        _, task_id, _ = parse_array(text, var_by_field["task_id"])
        _, suc_cnt, _ = parse_array(text, var_by_field["suc_cnt"])
        _, suc_idx, _ = parse_array(text, var_by_field["suc_idx"])
        _, successors, _ = parse_array(text, var_by_field["successors"])
        group_task_ids.append(task_id)
        group_suc.append((suc_cnt, suc_idx, successors))

    all_ids = sorted({tid for ids in group_task_ids for tid in ids})

    edges = []
    for suc_cnt, suc_idx, successors in group_suc:
        for pos in range(len(suc_cnt)):
            p = all_ids[pos]
            cnt = suc_cnt[pos]
            idx = suc_idx[pos]
            for s in successors[idx : idx + cnt]:
                edges.append((p, s))
    return edges


def to_mermaid(edges, title):
    lines = [f"## {title}", "```mermaid", "flowchart LR"]
    for a, b in sorted(set(edges)):
        lines.append(f"    {a} --> {b}")
    lines.append("```")
    return "\n".join(lines)


MERMAID_EDGE_LIMIT = 500  # mermaid's default maxEdges cap


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("generated", help="generated _suc.h file (has both predecessors and successors data)")
    ap.add_argument("-o", "--output", help="output .md path (default: <input, minus _suc>_graphs.md)")
    args = ap.parse_args()

    in_path = Path(args.generated)
    text = in_path.read_text(encoding="utf-8")

    pred_edges = edges_from_predecessors(text)
    suc_edges = edges_from_successors(text)
    match = set(pred_edges) == set(suc_edges)

    if args.output:
        out_path = Path(args.output)
    else:
        stem = in_path.stem[:-4] if in_path.stem.endswith("_suc") else in_path.stem
        out_path = in_path.with_name(stem + "_graphs.md")

    summary = (
        f"**Edge check**: {len(pred_edges)} predecessor-edges vs {len(suc_edges)} successor-edges "
        f"-> **{'MATCH' if match else 'DIFFER'}**\n\n"
        + ("Edge sets are identical, so the two rendered graphs would necessarily be identical too.\n"
           if match else
           "Edge sets differ - the two graphs below (if rendered) would NOT look the same.\n")
    )

    too_big = max(len(pred_edges), len(suc_edges)) > MERMAID_EDGE_LIMIT
    parts = [summary]
    if too_big:
        parts.append(
            f"Note: {max(len(pred_edges), len(suc_edges))} edges exceeds mermaid's default `maxEdges` "
            f"config of {MERMAID_EDGE_LIMIT}. If the diagrams below don't render in your viewer, that's "
            "why - it's a configurable default (`mermaid.initialize({maxEdges: ...})`), not a hard cap, "
            "so a different renderer or a raised config may still show it. The edge-check above already "
            "tells you what the picture would show either way."
        )
    parts += [
        to_mermaid(pred_edges, "predecessors view (rebuilt from the original file)"),
        "",
        to_mermaid(suc_edges, "successors view (rebuilt from the generated file)"),
    ]
    out_path.write_text("\n".join(parts) + "\n", encoding="utf-8")

    print(f"wrote {out_path}: {len(pred_edges)} predecessor-edges, {len(suc_edges)} successor-edges, "
          f"edge sets {'MATCH' if match else 'DIFFER'}"
          + (" (over mermaid's default render limit, may not render everywhere)" if too_big else ""))


if __name__ == "__main__":
    main()
