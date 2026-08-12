#!/bin/bash
# Batch-run inject_successors.py over multiple case headers.
# Add one line per file; each can have its own output path.
set -e

python inject_successors.py /path/to/paged_attention_subgraph.h -o /path/to/paged_attention_subgraph_suc.h
python inject_successors.py /path/to/qwen3_14b_decode_subgraph.h -o /path/to/qwen3_14b_decode_subgraph_suc.h
