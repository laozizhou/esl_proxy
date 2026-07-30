#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_FILE="${LOG_FILE:-log/a11.log}"
A11_DUMP_PERIOD="${A11_DUMP_PERIOD:-100}"
mkdir -p "$(dirname "$LOG_FILE")"

make -s clean >/dev/null 2>&1
make -s all CASE=ed_a11_probe.h ED_ENABLE=1 \
    EXTRA_CFLAGS="-DED_A11_PROBE=1 -DED_A11_DUMP_PERIOD=${A11_DUMP_PERIOD}" >/dev/null 2>&1
WORKER_LOG=1 ./bin/esl_proxy >"$LOG_FILE" 2>&1

s_id="$(awk -F'probe_s=' '/\[a11\] probe_s=/{print $2; exit}' "$LOG_FILE" | awk '{print $1}')"
test -n "$s_id" || { echo "FAIL A11.0 missing [a11] probe_s"; exit 1; }

awk -v s_id="$s_id" -F'[ ,]+' '
$0 ~ ("notify_write, s=" s_id ",") { seen_notify = 1 }
$0 ~ ("slot_state_dump, s=" s_id ",") {
    seen = 1
    unfin = -1; spec = -1; state = -1; db = -1
    for (i = 1; i <= NF; i++) {
        n = index($i, "=")
        if (n == 0) continue
        k = substr($i, 1, n - 1)
        v = substr($i, n + 1) + 0
        if      (k == "unfin")    unfin = v
        else if (k == "spec")     spec = v
        else if (k == "state")    state = v
        else if (k == "doorbell") db = v
    }
    if (unfin < 0 || spec < 0 || state < 0 || db < 0) {
        print "FAIL A11.x malformed dump:", $0
        f++
        next
    }
    if (unfin > 0) {
        seen_wait = 1
        if (spec != 1)  { print "FAIL A11.1", $0; f++ }
        if (state != 1) { print "FAIL A11.2", $0; f++ }
        if (db != 0)    { print "FAIL A11.3", $0; f++ }
    }
    if (unfin == 0) {
        seen_release = 1
        if (state != 2) { print "FAIL A11.4", $0; f++ }
    }
}
END {
    if (!seen)         { print "FAIL A11.0 no slot_state_dump for s=" s_id; f++ }
    if (!seen_wait)    { print "FAIL A11.0 no unfin>0 sample for s=" s_id; f++ }
    if (!seen_release && !seen_notify) {
        print "FAIL A11.4 no release evidence for s=" s_id
        f++
    }
    exit (f ? 1 : 0)
}' "$LOG_FILE"

echo "A11 PASS"
