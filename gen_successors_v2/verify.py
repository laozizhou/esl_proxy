import re
import sys

ARRAY_RE = re.compile(r"static\s+\S+\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}\s*;", re.DOTALL)
STRUCT_RE = re.compile(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*\1\s*;", re.DOTALL)


def parse_arrays(text):
    arrays = {}
    for name, body in ARRAY_RE.findall(text):
        arrays[name] = [int(x) for x in body.split(",") if x.strip() != ""]
    return arrays


def parse_fields(text):
    m = STRUCT_RE.search(text)
    fields = []
    for line in m.group(2).split(";"):
        line = line.strip()
        if not line:
            continue
        tokens = line.replace("*", " * ").split()
        fields.append(tokens[-1])
    return fields


def find_matching_brace(text, open_pos):
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i


def parse_entries(text, struct_name):
    m = re.search(r"static\s+" + struct_name + r"\s+\w+\s*\[[^\]]*\]\s*=\s*\{", text)
    open_pos = m.end() - 1
    close_pos = find_matching_brace(text, open_pos)
    body = text[open_pos + 1 : close_pos]
    entries = []
    depth = 0
    start = None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                entries.append(body[start + 1 : i])
    return entries


def split_top(s):
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def verify(path):
    text = open(path).read()
    arrays = parse_arrays(text)
    struct_name = STRUCT_RE.search(text).group(1)
    fields = parse_fields(text)
    entries = parse_entries(text, struct_name)

    groups = []
    for e in entries:
        tokens = split_top(e)
        var_by_field = dict(zip(fields, tokens))
        groups.append({
            "task_id": arrays[var_by_field["task_id"]],
            "pre_cnt": arrays[var_by_field["pre_cnt"]],
            "pre_idx": arrays[var_by_field["pre_idx"]],
            "predecessors": arrays[var_by_field["predecessors"]],
            "suc_cnt": arrays[var_by_field["suc_cnt"]],
            "suc_idx": arrays[var_by_field["suc_idx"]],
            "successors": arrays[var_by_field["successors"]],
            "type": arrays[var_by_field["type"]],
            "suc_type": arrays[var_by_field["suc_type"]],
        })

    group_of_id = {}
    for g, grp in enumerate(groups):
        for tid in grp["task_id"]:
            group_of_id[tid] = g
    id_order = sorted(group_of_id.keys())
    pos_of = {tid: i for i, tid in enumerate(id_order)}
    total_task_cnt = len(id_order)

    # Rebuild expected edges: (predecessor_id, successor_id, successor_group)
    from collections import Counter
    expected = Counter()  # (target_group, predecessor_pos) -> Counter of successor_id
    expected_lists = {}
    for g, grp in enumerate(groups):
        for i, tid in enumerate(grp["task_id"]):
            for k in range(grp["pre_cnt"][i]):
                p = grp["predecessors"][grp["pre_idx"][i] + k]
                if p not in pos_of:
                    continue
                key = (g, pos_of[p])
                expected_lists.setdefault(key, Counter())[tid] += 1

    mismatches = 0
    total_expected_edges = sum(sum(c.values()) for c in expected_lists.values())
    total_actual_edges = 0

    for g, grp in enumerate(groups):
        if len(grp["suc_cnt"]) != total_task_cnt or len(grp["suc_idx"]) != total_task_cnt:
            print(f"  FAIL group{g}: suc_cnt/suc_idx length != total_task_cnt ({total_task_cnt})")
            mismatches += 1
            continue
        # check suc_idx is proper prefix sum of suc_cnt
        running = 0
        for pos in range(total_task_cnt):
            if grp["suc_idx"][pos] != running:
                print(f"  FAIL group{g} pos{pos}: suc_idx={grp['suc_idx'][pos]} expected prefix-sum={running}")
                mismatches += 1
            running += grp["suc_cnt"][pos]
        for pos in range(total_task_cnt):
            cnt = grp["suc_cnt"][pos]
            idx = grp["suc_idx"][pos]
            actual = Counter(grp["successors"][idx : idx + cnt])
            total_actual_edges += cnt
            expected_c = expected_lists.get((g, pos), Counter())
            if actual != expected_c:
                print(f"  FAIL group{g} pos{pos} (task_id={id_order[pos]}): actual={dict(actual)} expected={dict(expected_c)}")
                mismatches += 1

    # suc_type: build the expected merged type-by-position list, then check
    # every group's suc_type_N matches it exactly (content must be identical
    # across all groups, and equal to the owning group's own `type` value).
    expected_type = [None] * total_task_cnt
    for grp in groups:
        for i, tid in enumerate(grp["task_id"]):
            expected_type[pos_of[tid]] = grp["type"][i]

    for g, grp in enumerate(groups):
        if grp["suc_type"] != expected_type:
            print(f"  FAIL group{g}: suc_type does not match expected merged type list")
            for pos in range(total_task_cnt):
                if grp["suc_type"][pos] != expected_type[pos]:
                    print(f"    pos{pos} (task_id={id_order[pos]}): suc_type={grp['suc_type'][pos]} expected={expected_type[pos]}")
            mismatches += 1

    print(f"  total_task_cnt={total_task_cnt}, expected_edges={total_expected_edges}, actual_edges_in_output={total_actual_edges}, mismatches={mismatches}")
    return mismatches == 0 and total_expected_edges == total_actual_edges


if __name__ == "__main__":
    files = sys.argv[1:]
    all_ok = True
    for f in files:
        print(f"=== {f} ===")
        ok = verify(f)
        print("  RESULT:", "PASS" if ok else "FAIL")
        all_ok = all_ok and ok
    print()
    print("ALL PASS" if all_ok else "SOME FAILED")
