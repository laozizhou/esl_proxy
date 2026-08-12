#!/usr/bin/env python3
"""
batch_inject_successors.py [root_dir]

Recursively walks root_dir (default: current directory) for *.h files, and
runs inject_successors.process_file on each one that has the expected
`subgraph` struct + `test_graph[]` shape. Each match gets a sibling
<name>_suc.h written next to it (same rule as running inject_successors.py
by hand, one file at a time). No input filenames need to be given.

Files that don't match the expected shape are skipped and reported, not
silently ignored. Previously-generated `*_suc.h` files are skipped so
re-running this doesn't chain into `*_suc_suc.h`.
"""
import sys
from pathlib import Path

from inject_successors import process_file


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")

    processed = []
    skipped = []
    for h_file in sorted(root.rglob("*.h")):
        if h_file.stem.endswith("_suc"):
            continue
        try:
            out_path, total_local, total_edges, n_groups = process_file(h_file)
            processed.append(h_file)
            print(f"OK   {h_file} -> {out_path}: {total_local}/{total_edges} edges, {n_groups} group(s)")
        except Exception as e:
            skipped.append((h_file, e))
            print(f"SKIP {h_file}: {e}", file=sys.stderr)

    print(f"\n{len(processed)} file(s) processed, {len(skipped)} skipped")


if __name__ == "__main__":
    main()
