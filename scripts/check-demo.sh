#!/usr/bin/env bash
# Checks a running RaftKV hosted demo from the outside, the way a browser and the
# dashboard would use it. Read-only apart from one throwaway key, which it deletes.
# Nothing is crashed and no rate limit is pushed, so it is safe to run against a live demo.
#
#   scripts/check-demo.sh <backend-url> [dashboard-origin]
#
#   scripts/check-demo.sh https://raftkv-demo.onrender.com https://raftkv.vercel.app
#   scripts/check-demo.sh http://localhost:10000 http://localhost:5173
#
# With a dashboard origin it also checks CORS for that exact origin. WAIT_SECONDS (default
# 120) is how long to wait for a cold start (a sleeping free-tier service takes a while).
# Needs curl. Exits non-zero if any check fails.

set -uo pipefail

BASE="${1:?usage: check-demo.sh <backend-url> [dashboard-origin]}"
BASE="${BASE%/}"
ORIGIN="${2:-}"
ORIGIN="${ORIGIN%/}"
WAIT="${WAIT_SECONDS:-120}"
KEY="check-demo-$$"

passed=0
failed=0
ok()  { echo "PASS  $1"; passed=$((passed + 1)); }
bad() { echo "FAIL  $1${2:+  ($2)}"; failed=$((failed + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', expected '$3'"; fi; }
http() { curl -s -m 10 -o /dev/null -w '%{http_code}' "$@" 2>/dev/null || echo 000; }

# 1. The service answers (waiting out a cold start).
up=0
end=$((SECONDS + WAIT))
while [ "$SECONDS" -lt "$end" ]; do
    [ "$(http "$BASE/healthz")" = "200" ] && { up=1; break; }
    sleep 2
done
if [ "$up" != 1 ]; then
    bad "service answers /healthz within ${WAIT}s" "is the URL right, and is the service running?"
    echo; echo "RESULT: $passed passed, $failed failed"; exit 1
fi
ok "service answers /healthz"

# 2. A leader is elected and visible through the gateway.
leader=""
end=$((SECONDS + 60))
while [ "$SECONDS" -lt "$end" ] && [ -z "$leader" ]; do
    for k in 1 2 3; do
        role="$(curl -s -m 5 "$BASE/n/$k/status" | sed -n 's/.*"role":"\([a-z]*\)".*/\1/p')"
        [ "$role" = "leader" ] && leader="$k"
    done
    [ -z "$leader" ] && sleep 1
done
if [ -z "$leader" ]; then
    bad "a leader is elected within 60s" "all three nodes should be up; check the service logs"
    echo; echo "RESULT: $passed passed, $failed failed"; exit 1
fi
ok "a leader is elected (node-$leader)"
follower=$((leader % 3 + 1))

# 3. A write sent to a follower is redirected back through THIS URL, and succeeds.
location="$(curl -s -m 10 -o /dev/null -w '%{redirect_url}' -X PUT --data-binary hello "$BASE/n/$follower/kv/$KEY")"
check "follower redirects to the leader through the public URL" "$location" "$BASE/n/$leader/kv/$KEY"
check "write succeeds after following the redirect" \
    "$(http -L -X PUT --data-binary hello "$BASE/n/$follower/kv/$KEY")" "204"
check "value reads back through a different node" \
    "$(curl -sL -m 10 "$BASE/n/$follower/kv/$KEY")" "hello"
http -L -X DELETE "$BASE/n/$leader/kv/$KEY" >/dev/null

# 4. Only the dashboard's routes are reachable.
check "peer RPC (/raft/append-entries) is not reachable" \
    "$(http -X POST "$BASE/n/1/raft/append-entries")" "404"
check "peer RPC (/raft/request-vote) is not reachable" \
    "$(http -X POST "$BASE/n/1/raft/request-vote")" "404"
check "stopping the whole cluster is not reachable" \
    "$(http -X POST "$BASE/launcher/cluster/stop")" "404"

# 5. The launcher controls the dashboard needs are there.
nodes="$(curl -s -m 10 "$BASE/launcher/nodes")"
running="$(echo "$nodes" | grep -o '"running":true' | wc -l | tr -d ' ')"
check "launcher reports all three nodes running" "$running" "3"
check "launcher reports network state" "$(http "$BASE/launcher/network")" "200"

# 6. A live stream delivers state (read for a few seconds, then stop).
stream="$(curl -sN -m 4 "$BASE/n/1/cluster/events" 2>/dev/null || true)"
echo "$stream" | grep -q '^event: cluster' && ok "live cluster stream delivers state" \
    || bad "live cluster stream delivers state" "no 'event: cluster' within 4s"

# 7. CORS, for the dashboard's exact origin.
if [ -n "$ORIGIN" ]; then
    header() { tr -d '\r' | grep -i "^$1:" | head -1 | sed 's/^[^:]*: *//'; }

    got="$(curl -s -m 10 -D - -o /dev/null -H "Origin: $ORIGIN" "$BASE/n/1/status" | header access-control-allow-origin)"
    check "CORS allows the dashboard origin on GET" "$got" "$ORIGIN"

    pre="$(curl -s -m 10 -D - -o /dev/null -X OPTIONS \
        -H "Origin: $ORIGIN" -H "Access-Control-Request-Method: PUT" \
        -H "Access-Control-Request-Headers: content-type" "$BASE/n/1/kv/$KEY")"
    check "CORS preflight for a write succeeds" "$(echo "$pre" | head -1 | awk '{print $2}')" "204"
    check "CORS preflight allows the dashboard origin" "$(echo "$pre" | header access-control-allow-origin)" "$ORIGIN"

    other="$(curl -s -m 10 -D - -o /dev/null -H "Origin: https://not-allowed.invalid" "$BASE/n/1/status" | header access-control-allow-origin)"
    check "CORS refuses an unrelated origin" "$other" ""
else
    echo "SKIP  CORS checks (no dashboard origin given)"
fi

echo
echo "RESULT: $passed passed, $failed failed"
[ "$failed" = 0 ]
