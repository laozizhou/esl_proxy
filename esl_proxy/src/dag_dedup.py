"""
Compute how many edges in dag_raw.csv (produced by dag_export.c) are
transitively redundant, and write out a deduplicated copy of the DAG (same
CSV format, redundant predecessors removed).

Usage: python src/dag_dedup.py [input.csv] [output.csv]
  input.csv defaults to "dag_raw.csv"
  output.csv defaults to "dag_deduped.csv"

input.csv columns: task_id,type,duration,predecessors (';'-separated)
Task IDs are a valid topological order (predecessors always have smaller id,
by construction of the ring-buffer task_id counter), so we can compute
ancestor sets in a single forward pass.

For each task v with direct predecessors P(v):
  edge (p -> v) is redundant iff p is an ancestor of some OTHER direct
  predecessor q of v (q != p) -- i.e. there's already a path p -> ... -> q -> v,
  so the direct p -> v edge adds no new ordering constraint.
"""
import csv
import sys

def main(path, out_path):
    rows = {}  # task_id -> (type, duration, [predecessor ids])
    max_id = -1
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        next(reader)  # header
        for row in reader:
            tid = int(row[0])
            preds_str = row[3]
            preds = [int(x) for x in preds_str.split(';')] if preds_str else []
            rows[tid] = (row[1], row[2], preds)
            max_id = max(max_id, tid)

    n = max_id + 1
    ancestors = [0] * n       # bitset per task: full transitive ancestor set
    kept_preds = [None] * n   # predecessor list with redundant edges removed

    total_edges = 0
    redundant_edges = 0
    redundant_by_task = {}

    for tid in range(n):
        _, _, preds = rows.get(tid, ("0", "0", []))
        total_edges += len(preds)

        # union of {p} | ancestors[p] for all direct predecessors p
        acc = 0
        for p in preds:
            acc |= (1 << p) | ancestors[p]

        # check each predecessor against the union of *other* predecessors' ancestor sets
        red_count = 0
        survivors = []
        for p in preds:
            others_union = 0
            for q in preds:
                if q != p:
                    others_union |= ancestors[q]
            if (others_union >> p) & 1:
                redundant_edges += 1
                red_count += 1
            else:
                survivors.append(p)
        if red_count:
            redundant_by_task[tid] = (red_count, len(preds))

        kept_preds[tid] = survivors
        ancestors[tid] = acc

    print(f"total tasks: {n}")
    print(f"total edges: {total_edges}")
    print(f"redundant edges: {redundant_edges}")
    print(f"redundant ratio: {redundant_edges/total_edges*100:.2f}%")
    print(f"tasks with at least one redundant predecessor: {len(redundant_by_task)}")

    with open(out_path, "w", newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(["task_id", "type", "duration", "predecessors"])
        for tid in range(n):
            type_, duration, _ = rows.get(tid, ("0", "0", []))
            writer.writerow([tid, type_, duration, ";".join(str(p) for p in kept_preds[tid])])
    print(f"wrote deduplicated DAG to {out_path} ({sum(len(p) for p in kept_preds)} edges)")

if __name__ == "__main__":
    in_path = sys.argv[1] if len(sys.argv) > 1 else "dag_raw.csv"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "dag_deduped.csv"
    main(in_path, out_path)
