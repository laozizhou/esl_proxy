#!/usr/bin/env python3
"""Generate compile_commands.json for clangd / IDE IntelliSense."""

from __future__ import annotations

import json
import os
import shlex
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CASE = os.environ.get("CASE", "qwen3_dynamic_manual_scope.h")
QWEN3_SPMD_TIER = os.environ.get("QWEN3_SPMD_TIER", "2")

QWEN3_CASES = {
    "qwen3_dynamic_manual_scope.h",
    "qwen3_dynamic_tensormap.h",
}

CFLAGS = [
    "-g",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-O2",
    "-D_POSIX_C_SOURCE=199309L",
    "-I",
    "include/algorithm",
    "-I",
    "cases",
    f"-DORCH_CASE={CASE}",
    "-MMD",
    "-MP",
    "-Wno-error=implicit-function-declaration",
]
if CASE in QWEN3_CASES:
    CFLAGS.append(f"-DQWEN3_SPMD_TIER={QWEN3_SPMD_TIER}")

SRCS = [
    "src/main.c",
    "src/algorithm/executor.c",
    "src/algorithm/dispatch.c",
    "src/algorithm/cutter.c",
    "src/algorithm/manager.c",
    "src/algorithm/log.c",
    "src/algorithm/shm.c",
]


def main() -> None:
    entries = []
    for src in SRCS:
        rel_obj = Path("build/obj") / Path(src).relative_to("src").with_suffix(".o")
        cmd_parts = ["cc", *CFLAGS, "-c", src, "-o", str(rel_obj)]
        entries.append(
            {
                "directory": str(ROOT),
                "command": " ".join(shlex.quote(part) for part in cmd_parts),
                "file": str(ROOT / src),
            }
        )

    out = ROOT / "compile_commands.json"
    out.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out} (CASE={CASE})")


if __name__ == "__main__":
    main()
