#!/usr/bin/env python3
"""
Generate a synthetic DAG subgraph header file (like paged_attention_subgraph.h).

Parameters:
  --tasks-per-layer  Tasks per layer (breadth); total = tasks_per_layer * chain_depth
  --pre-cnt          Number of predecessors each layer-k task has from layer-(k-1).
                    Layer 0 tasks have 0 predecessors.
  --chain-depth      Dependency chain depth; total tasks = tasks_per_layer * chain_depth
  --thread-cnt       Number of painter threads / subgraphs (PAINTER_THREAD_CNT)
  --avg-duration     Average task execution time in ns (default: 30000)
  --output           Output .h file path (default: stdout)
  --name             Workload name prefix (default: "synthetic")
  --dot              Optional output DOT file for visualization
  --seed             Random seed for durations (default: 42)
"""

import argparse
import random
import os
import re
import subprocess
import sys
from typing import Optional


def format_int_array(name: str, values: list[int], per_line: int = 30) -> str:
    """Format a C static array initializer."""
    body = ", ".join(str(v) for v in values)
    return f"static int {name}[{len(values)}] = {{{body}}};"


def format_uint_array(name: str, values: list[int], per_line: int = 30) -> str:
    """Format a C static uint32_t array initializer."""
    body = ", ".join(str(v) for v in values)
    return f"static uint32_t {name}[{len(values)}] = {{{body}}};"


def format_char_array(name: str, values: list[int]) -> str:
    """Format a C static char array initializer."""
    body = ", ".join(str(v) for v in values)
    return f"static char {name}[{len(values)}] = {{{body}}};"


def generate_task_ids(task_cnt: int, chain_depth: int, thread_idx: int, thread_cnt: int) -> list[int]:
    """Generate interleaved task IDs for one subgraph.

    Tasks are grouped into `chain_depth` waves, each wave of size
    `tasks_per_layer` tasks, split evenly among threads.
    Thread `thread_idx` gets a contiguous slice of size
    tasks_per_thread_per_wave.

    Example (task_cnt=32, chain_depth=1, thread_cnt=2, thread_idx=0):
        wave 0: [0..15] → sg0 gets [0..15], sg1 gets [16..31]
    """
    tasks_per_wave = task_cnt // chain_depth
    per_thread = tasks_per_wave // thread_cnt
    ids = []
    for wave in range(chain_depth):
        base = wave * tasks_per_wave + thread_idx * per_thread
        for i in range(per_thread):
            ids.append(base + i)
    return ids


def generate_pre_cnt_data(
    task_ids: list[int],
    pre_cnt: int,
    per_sub_layer: int,
    chain_depth: int,
) -> tuple[list[int], list[int], list[int]]:
    """Generate pre_cnt, pre_idx, predecessors arrays.

    Layer-to-layer dependency: tasks in layer k (k>0) each depend on
    ``pre_cnt`` tasks from layer k-1 (wrapping within the layer).

    Returns (pre_cnt_list, pre_idx_list, predecessors_list).
    """
    n = len(task_ids)  # == per_sub_layer * chain_depth

    pre_cnt_list = [0] * n
    pre_idx_list = [0] * n
    predecessors_list = []

    offset = 0
    for i in range(n):
        pre_idx_list[i] = offset
        layer = i // per_sub_layer
        if layer == 0:
            pre_cnt_list[i] = 0
        else:
            pos_in_layer = i % per_sub_layer
            cnt = min(pre_cnt, per_sub_layer)
            pre_cnt_list[i] = cnt
            for k in range(cnt):
                pred_pos = (pos_in_layer + k) % per_sub_layer
                pred_idx = (layer - 1) * per_sub_layer + pred_pos
                predecessors_list.append(task_ids[pred_idx])
            offset += cnt

    return pre_cnt_list, pre_idx_list, predecessors_list


def generate_type_array(n: int) -> list[int]:
    """Alternate aic(1) / aiv(0)."""
    return [1 if i % 2 == 0 else 0 for i in range(n)]


def generate_durations(n: int, avg_duration: int, seed: int = 42) -> list[int]:
    """Generate synthetic durations (ns) around avg_duration.

    AIC tasks (even index) get longer durations: avg_duration * [0.9, 1.6]
    AIV tasks (odd index)  get shorter durations: avg_duration * [0.04, 1.0]
    This mimics the paged_attention pattern where AIC ≈ 50k and AIV ≈ 2.5k.
    """
    rng = random.Random(seed)
    durations = []
    for i in range(n):
        if i % 2 == 0:  # AIC
            factor = rng.uniform(0.9, 1.6)
        else:           # AIV
            factor = rng.uniform(0.04, 1.0)
        durations.append(max(1, int(avg_duration * factor)))
    return durations


def indent(text: str, spaces: int = 2) -> str:
    """Indent each line by given spaces."""
    prefix = " " * spaces
    return prefix + text.replace("\n", "\n" + prefix)


def generate_header(args: argparse.Namespace) -> str:
    chain_depth = args.chain_depth
    pre_cnt_p = args.pre_cnt
    thread_cnt = args.thread_cnt
    name = args.name
    avg_duration = args.avg_duration

    tasks_per_layer = args.tasks_per_layer
    task_cnt = tasks_per_layer * chain_depth

    # Validate
    if tasks_per_layer % thread_cnt != 0:
        raise ValueError(
            f"tasks_per_layer {tasks_per_layer} must be divisible by thread_cnt {thread_cnt}"
        )

    lines = []
    guard = f"CASES_STATIC_{name.upper()}_SUBGRAPH_H"

    lines.append("/*")
    lines.append(f" * AUTO-GENERATED by tools/gen_synthetic_dag.py.")
    lines.append(f" * workload: {name}, tasks: {task_cnt}.")
    lines.append(f" * {thread_cnt} interleaved subgraphs (tasks_per_wave={tasks_per_layer}); PAINTER_THREAD_CNT={thread_cnt}.")
    lines.append(f" * pre_cnt: {pre_cnt_p}, chain_depth={chain_depth}, tasks_per_layer={tasks_per_layer}.")
    lines.append(f" * avg_duration: {avg_duration} ns.")
    lines.append(" */")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append('#include "scheduler/conf.h"')
    lines.append("")
    lines.append(f"static uint32_t total_task_cnt = {task_cnt};")
    lines.append("")

    # Generate each subgraph
    all_task_ids = []
    all_types = []
    all_durations = []
    all_pre_cnt = []
    all_pre_idx = []
    all_predecessors = []

    for t in range(thread_cnt):
        sg_name = f"sg{t}"
        ids = generate_task_ids(task_cnt, chain_depth, t, thread_cnt)
        n_sub = len(ids)
        types = generate_type_array(n_sub)
        durations = generate_durations(n_sub, avg_duration, seed=args.seed + t)
        per_sub_layer = tasks_per_layer // thread_cnt
        pre_cnt, pre_idx, predecessors = generate_pre_cnt_data(ids, pre_cnt_p, per_sub_layer, chain_depth)

        all_task_ids.append(ids)
        all_types.append(types)
        all_durations.append(durations)
        all_pre_cnt.append(pre_cnt)
        all_pre_idx.append(pre_idx)
        all_predecessors.append(predecessors)

        lines.append(f"/* ---- subgraph {t}: interleaved waves (global ids) ---- */")
        lines.append(f"#define {name.upper()}_SG{t}_TASK_CNT {n_sub}")
        lines.append("")
        # task_id
        lines.append(indent(format_uint_array(f"{sg_name}_task_id", ids)))
        # type
        lines.append(indent(format_char_array(f"{sg_name}_type", types)))
        # duration
        lines.append(indent(format_int_array(f"{sg_name}_duration", durations)))
        # pre_cnt
        lines.append(indent(format_int_array(f"{sg_name}_pre_cnt", pre_cnt)))
        # pre_idx
        lines.append(indent(format_int_array(f"{sg_name}_pre_idx", pre_idx)))
        # predecessors
        if predecessors:
            lines.append(indent(format_int_array(f"{sg_name}_predecessors", predecessors)))
        else:
            lines.append(f"static int {sg_name}_predecessors[] = {{0}};")
        lines.append("")

    # subgraph struct
    lines.append("typedef struct subgraph {")
    lines.append("    uint32_t  task_cnt;")
    lines.append("    uint32_t* task_id;")
    lines.append("    char*     type;          /* aiv=0, aic=1, mix=2 */")
    lines.append("    int*      duration;")
    lines.append("    int*      pre_cnt;")
    lines.append("    int*      pre_idx;")
    lines.append("    int*      predecessors;")
    lines.append("} subgraph;")
    lines.append("")

    if thread_cnt != 2:
        lines.append(f"#if PAINTER_THREAD_CNT != {thread_cnt}")
        lines.append(f'#error "subgraph header expects PAINTER_THREAD_CNT=={thread_cnt}"')
        lines.append("#endif")
    else:
        lines.append("#if PAINTER_THREAD_CNT != 2")
        lines.append('#error "subgraph header expects PAINTER_THREAD_CNT==2"')
        lines.append("#endif")

    # graph array
    lines.append(f"static subgraph test_graph[PAINTER_THREAD_CNT] = {{")
    for t in range(thread_cnt):
        sg_name = f"sg{t}"
        n_sub = len(all_task_ids[t])
        line = f"    {{{n_sub}, {sg_name}_task_id, {sg_name}_type, {sg_name}_duration, {sg_name}_pre_cnt, {sg_name}_pre_idx, {sg_name}_predecessors}}"
        if t < thread_cnt - 1:
            line += ","
        lines.append(line)
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")

    return "\n".join(lines) + "\n"


def render_dot(dot_path: str, output_path: str, fmt: str = "png") -> bool:
    """Render DOT file to image using Graphviz."""
    try:
        result = subprocess.run(["which", "dot"], capture_output=True, text=True)
        if result.returncode != 0:
            print(
                "Warning: Graphviz 'dot' command not found. "
                "Install with: brew install graphviz"
            )
            print(f"Generated DOT file: {dot_path}")
            return False

        cmd = ["dot", "-T", fmt, "-o", output_path, dot_path]
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode == 0:
            print(f"Rendered image saved to: {output_path}")
            return True
        else:
            print(f"Error rendering DOT: {result.stderr}")
            return False

    except Exception as e:
        print(f"Error rendering DOT: {e}")
        return False


def _extract_c_array(text: str, var_prefix: str) -> Optional[list[int]]:
    """Extract integer values from a C static array initializer like ``sg0_task_id[720] = {0,1,2};``."""
    pattern = re.compile(rf"\b{re.escape(var_prefix)}\s*\[[^\]]*\]\s*=\s*\{{\s*([^}}]*)\}}\s*;", re.DOTALL)
    m = pattern.search(text)
    if not m:
        return None
    body = m.group(1)
    values = [int(v.strip()) for v in body.split(",") if v.strip()]
    return values


def parse_header(header_path: str) -> tuple[list[list[int]], list[list[int]], list[list[int]], list[list[int]], int]:
    """Parse a generated .h subgraph header file.

    Returns:
        task_ids_per_sg:  list of lists – task_id arrays per subgraph
        types_per_sg:     list of lists – type arrays per subgraph
        pre_cnts_per_sg:  list of lists – pre_cnt arrays per subgraph
        preds_per_sg:     list of lists – predecessor arrays per subgraph
        thread_cnt:       number of subgraphs detected
    """
    with open(header_path, "r") as f:
        content = f.read()

    task_ids_per_sg = []
    types_per_sg = []
    pre_cnts_per_sg = []
    preds_per_sg = []
    t = 0

    while True:
        ids = _extract_c_array(content, f"sg{t}_task_id")
        if ids is None:
            break
        typs = _extract_c_array(content, f"sg{t}_type")
        pre_cnts = _extract_c_array(content, f"sg{t}_pre_cnt")
        preds = _extract_c_array(content, f"sg{t}_predecessors")
        task_ids_per_sg.append(ids)
        types_per_sg.append(typs)
        pre_cnts_per_sg.append(pre_cnts if pre_cnts is not None else [])
        preds_per_sg.append(preds if preds is not None else [])
        t += 1

    if t == 0:
        raise ValueError(f"No subgraph data found in {header_path}")

    return task_ids_per_sg, types_per_sg, pre_cnts_per_sg, preds_per_sg, t


def generate_dot_from_data(
    task_ids_per_sg: list[list[int]],
    types_per_sg: list[list[int]],
    predecessors_per_sg: list[list[int]],
    pre_cnts_per_sg: Optional[list[list[int]]] = None,
    graph_name: str = "DAG",
) -> str:
    """Generate DOT content from parsed subgraph arrays."""
    TASK_TYPE_NAMES = {0: "AIV", 1: "AIC", 2: "MIX"}
    SHAPE_MAP = {0: "ellipse", 1: "rect", 2: "diamond"}
    COLOR_MAP = {0: "#9c27b0", 1: "#ff9800", 2: "#00bcd4"}

    thread_cnt = len(task_ids_per_sg)

    dot_lines = [
        f"digraph {graph_name} {{",
        "  rankdir=TB;",
        '  bgcolor="#ffffff";',
        '  node [fontname="Arial" fontcolor="#000000"];',
        '  edge [color="#666" fontname="Arial"];',
        "",
    ]

    # Build id → type mapping and edges from predecessors arrays
    all_nodes: dict[int, int] = {}  # node_id → type
    edges: set[tuple[int, int]] = set()

    for sg_idx in range(thread_cnt):
        ids = task_ids_per_sg[sg_idx]
        typs = types_per_sg[sg_idx] if types_per_sg[sg_idx] else [0] * len(ids)
        preds = predecessors_per_sg[sg_idx]
        pcnts = pre_cnts_per_sg[sg_idx] if pre_cnts_per_sg else None

        # Use pre_cnt (if available) to properly consume predecessor array
        pred_ptr = 0
        for local_i in range(len(ids)):
            node_id = ids[local_i]
            task_type = typs[local_i] if local_i < len(typs) else 0
            all_nodes[node_id] = task_type

            if pcnts is not None and local_i < len(pcnts):
                cnt = pcnts[local_i]
            else:
                # Fallback heuristic: if pred_ptr still has entries matching
                # the previous ID, consume one predecessor
                cnt = 0
                if local_i > 0 and pred_ptr < len(preds) and preds[pred_ptr] == ids[local_i - 1]:
                    cnt = 1

            for _ in range(cnt):
                if pred_ptr < len(preds):
                    edges.add((preds[pred_ptr], node_id))
                    pred_ptr += 1

    # Add nodes
    for node_id in sorted(all_nodes):
        task_type = all_nodes[node_id]
        type_name = TASK_TYPE_NAMES.get(task_type, "UNKNOWN")
        shape = SHAPE_MAP.get(task_type, "rect")
        color = COLOR_MAP.get(task_type, "#ff9800")
        dot_lines.append(
            f'  T{node_id} [label="T{node_id}\\n{type_name}" '
            f'shape={shape} fillcolor="{color}" fontcolor="#000000"];'
        )

    dot_lines.append("")

    for src, dst in sorted(edges):
        dot_lines.append(f"  T{src} -> T{dst};")

    dot_lines.append("}")
    return "\n".join(dot_lines) + "\n"


def generate_dot(args: argparse.Namespace) -> str:
    """Generate a DOT file for visualization of the DAG from parameters."""
    chain_depth = args.chain_depth
    tasks_per_layer = args.tasks_per_layer
    task_cnt = tasks_per_layer * chain_depth
    pre_cnt_p = args.pre_cnt
    thread_cnt = args.thread_cnt

    task_ids_per_sg = []
    types_per_sg = []
    pre_cnts_per_sg = []
    predecessors_per_sg = []

    for t in range(thread_cnt):
        ids = generate_task_ids(task_cnt, chain_depth, t, thread_cnt)
        n_sub = len(ids)
        types = generate_type_array(n_sub)
        per_sub_layer = tasks_per_layer // thread_cnt
        pre_cnt, pre_idx, predecessors = generate_pre_cnt_data(ids, pre_cnt_p, per_sub_layer, chain_depth)
        task_ids_per_sg.append(ids)
        types_per_sg.append(types)
        pre_cnts_per_sg.append(pre_cnt)
        predecessors_per_sg.append(predecessors)

    return generate_dot_from_data(
        task_ids_per_sg, types_per_sg, predecessors_per_sg,
        pre_cnts_per_sg=pre_cnts_per_sg, graph_name="SyntheticDAG",
    )


def main():
    parser = argparse.ArgumentParser(
        description="Generate a synthetic DAG subgraph header for the scheduler."
    )
    parser.add_argument(
        "--tasks-per-layer", "-t", type=int, default=32,
        help="Tasks per layer; total = tasks_per_layer * layers (default: 32)"
    )
    parser.add_argument(
        "--pre-cnt", "-p", type=int, default=3,
        help="Number of predecessors per task (sliding window). "
             "Task i depends on the min(i, p) preceding tasks. (default: 3)"
    )
    parser.add_argument(
        "--chain-depth", "-l", type=int, default=60,
        help="Dependency chain depth; total tasks = tasks_per_layer * chain_depth (default: 60)"
    )
    parser.add_argument(
        "--thread-cnt", "-c", type=int, default=2,
        help="PAINTER_THREAD_CNT / number of subgraphs (default: 2)"
    )
    parser.add_argument(
        "--output", "-o", type=str, default=None,
        help="Output .h file path (default: stdout)"
    )
    parser.add_argument(
        "--name", "-n", type=str, default="synthetic",
        help="Workload name prefix (default: synthetic)"
    )
    parser.add_argument(
        "--dot", type=str, default=None,
        help="Optional DOT output file for DAG visualization"
    )
    parser.add_argument(
        "--png", type=str, default=None,
        help="Optional PNG output file for DAG visualization (rendered via Graphviz)"
    )
    parser.add_argument(
        "--from-header", type=str, default=None,
        help="Parse an existing .h subgraph header and output DOT/PNG from it"
    )
    parser.add_argument(
        "--avg-duration", "-d", type=int, default=30000,
        help="Average task execution time in ns (default: 30000)"
    )
    parser.add_argument(
        "--seed", "-s", type=int, default=42,
        help="Random seed for duration jitter (default: 42)"
    )

    args = parser.parse_args()

    # --- from-header mode: parse existing .h and optionally visualize ---
    if args.from_header:
        if args.dot or args.png:
            task_ids_per_sg, types_per_sg, pre_cnts_per_sg, preds_per_sg, thread_cnt = parse_header(args.from_header)
            print(f"Parsed {len(task_ids_per_sg)} subgraphs from {args.from_header}")
            total = sum(len(ids) for ids in task_ids_per_sg)
            print(f"Total tasks: {total}, total edges: {sum(len(p) for p in preds_per_sg)}")

            graph_name = os.path.splitext(os.path.basename(args.from_header))[0]
            dot_content = generate_dot_from_data(
                task_ids_per_sg, types_per_sg, preds_per_sg,
                pre_cnts_per_sg=pre_cnts_per_sg, graph_name=graph_name,
            )
            dot_path = args.dot
            if not dot_path:
                dot_path = args.png.replace(".png", ".dot") if args.png and args.png.endswith(".png") else (args.png + ".dot" if args.png else None)
            if dot_path:
                with open(dot_path, "w") as f:
                    f.write(dot_content)
                print(f"DOT written to: {dot_path}")

            if args.png:
                render_dot(dot_path, args.png, "png")
        else:
            print("--from-header requires --dot or --png to specify an output file")
            sys.exit(1)
        return

    # --- generation mode ---
    header = generate_header(args)

    if args.output:
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        with open(args.output, "w") as f:
            f.write(header)
        print(f"Header written to: {args.output}")
    else:
        sys.stdout.write(header)

    if args.dot or args.png:
        dot_content = generate_dot(args)
        dot_path = args.dot
        if not dot_path:
            dot_path = args.png.replace(".png", ".dot") if args.png.endswith(".png") else args.png + ".dot"
        with open(dot_path, "w") as f:
            f.write(dot_content)
        print(f"DOT written to: {dot_path}")

        if args.png:
            render_dot(dot_path, args.png, "png")


if __name__ == "__main__":
    main()