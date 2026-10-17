#!/usr/bin/env bash
# Boots one real raftkv_backend node and exercises it end to end: status, a write,
# a read, a burst of concurrent writes, the live event stream, and a graceful
# SIGTERM shutdown. Fails if any step fails or if the node's log contains a
# sanitizer report.
#
# Used by .github/workflows/sanitizers.yml with sanitizer-instrumented builds, but it
# works on any build:
#
#   scripts/ci-smoke.sh backend/build/raftkv_backend
#
# Needs curl. Set PORT to change the port (default 8101).

set -euo pipefail

BIN="${1:?usage: ci-smoke.sh <path-to-raftkv_backend>}"
PORT="${PORT:-8101}"
URL="http://127.0.0.1:${PORT}"
BURST=20

WORK="$(mktemp -d)"
LOG="$WORK/node.log"
PID=""

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -KILL "$PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "SMOKE TEST FAILED: $1" >&2
    echo "--- node log ---" >&2
    cat "$LOG" >&2 || true
    exit 1
}

mkdir -p "$WORK/data"
"$BIN" --id ci-node --port "$PORT" --data "$WORK/data/ci-node" --no-repl >"$LOG" 2>&1 &
PID=$!

# 1. The node comes up and serves /status.
up=0
for _ in $(seq 1 40); do
    if curl -sf "$URL/status" >/dev/null 2>&1; then up=1; break; fi
    kill -0 "$PID" 2>/dev/null || fail "node exited during startup"
    sleep 0.5
done
[ "$up" = 1 ] || fail "node did not serve /status within 20 s"

# 2. A write is accepted. A lone node must first win its own election, so retry briefly.
code=000
for _ in $(seq 1 40); do
    code="$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary 'hello' "$URL/kv/greeting" || true)"
    [ "$code" = "204" ] && break
    sleep 0.5
done
[ "$code" = "204" ] || fail "PUT /kv/greeting returned $code, expected 204"

# 3. The value reads back (a linearizable read).
value="$(curl -sf "$URL/kv/greeting")" || fail "GET /kv/greeting failed"
[ "$value" = "hello" ] || fail "GET /kv/greeting returned '$value', expected 'hello'"

# 4. A burst of concurrent writes, so ThreadSanitizer sees real contention.
pids=()
for i in $(seq 1 "$BURST"); do
    (
        c="$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary "v$i" "$URL/kv/burst-$i" || true)"
        echo "$c" >"$WORK/burst-$i"
    ) &
    pids+=("$!")
done
wait "${pids[@]}"
for i in $(seq 1 "$BURST"); do
    [ "$(cat "$WORK/burst-$i")" = "204" ] || fail "concurrent PUT burst-$i returned $(cat "$WORK/burst-$i"), expected 204"
done
[ "$(curl -sf "$URL/kv/burst-$BURST")" = "v$BURST" ] || fail "burst write did not read back"

# 5. The live event stream sends its initial state.
timeout 3 curl -sN "$URL/events" >"$WORK/events.txt" 2>/dev/null || true
grep -q '^event: state' "$WORK/events.txt" || fail "/events did not send a state event"

# 6. SIGTERM shuts the node down cleanly.
kill -TERM "$PID"
for _ in $(seq 1 20); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.5
done
if kill -0 "$PID" 2>/dev/null; then
    fail "node did not exit within 10 s of SIGTERM"
fi
status=0
wait "$PID" || status=$?
PID=""
[ "$status" = 0 ] || fail "node exited with status $status after SIGTERM"

# 7. No sanitizer reported anything (each one ends a report with a SUMMARY line).
if grep -qE 'SUMMARY: [A-Za-z]*Sanitizer|runtime error:|WARNING: ThreadSanitizer' "$LOG"; then
    fail "a sanitizer reported an error"
fi

echo "smoke test passed: status, write, read, $BURST concurrent writes, event stream, clean shutdown"
echo "--- node log ---"
cat "$LOG"
