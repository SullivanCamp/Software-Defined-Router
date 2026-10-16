#!/usr/bin/env bash
#
# Non-interactive forwarding test for CI. Exits non-zero if the router fails to
# forward, fails to honor TTL, or fails to apply a live route change.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
CONF=/tmp/ci.conf
RPID=""
FAILURES=0

check() {
    if [[ $1 -eq 0 ]]; then
        echo "  PASS: $2"
    else
        echo "  FAIL: $2"
        FAILURES=$((FAILURES + 1))
    fi
}

cleanup() {
    [[ -n "$RPID" ]] && kill "$RPID" 2>/dev/null
    wait "$RPID" 2>/dev/null
    "$HERE/topology.sh" down >/dev/null 2>&1
}
trap cleanup EXIT

"$HERE/topology.sh" up          >/dev/null
"$HERE/topology.sh" conf "$CONF" >/dev/null

"$ROOT/build/sdrouter" -i rt1 -i rt2 -i rt3 -f "$CONF" > /tmp/ci-router.log 2>&1 &
RPID=$!
sleep 1

if ! kill -0 "$RPID" 2>/dev/null; then
    echo "router failed to start:"
    cat /tmp/ci-router.log
    exit 1
fi

echo "test: forwarding between subnets"
ip netns exec h1 ping -c 3 -W 2 10.0.2.10 >/dev/null 2>&1
check $? "h1 reaches h2 through the router"

ip netns exec h1 ping -c 2 -W 2 10.0.3.10 >/dev/null 2>&1
check $? "h1 reaches h3 through the router"

echo "test: ttl is decremented by sdrouter"
ttl=$(ip netns exec h1 ping -c 1 -W 2 10.0.2.10 2>/dev/null | grep -o 'ttl=[0-9]*' | head -1 | cut -d= -f2)
[[ "$ttl" == "63" ]]
check $? "reply ttl is 63, not 64 (got '${ttl:-none}')"

echo "test: ttl=1 is dropped"
ip netns exec h1 ping -c 1 -t 1 -W 1 10.0.2.10 >/dev/null 2>&1
[[ $? -ne 0 ]]
check $? "ttl-expired packet is not forwarded"

echo "test: live route withdrawal"
python3 "$ROOT/tools/rctl.py" del-route 10.0.2.0/24 >/dev/null 2>&1
check $? "DEL_ROUTE accepted"
sleep 0.5
ip netns exec h1 ping -c 1 -W 1 10.0.2.10 >/dev/null 2>&1
[[ $? -ne 0 ]]
check $? "traffic stops after the route is withdrawn"

echo "test: live route reinstall"
python3 "$ROOT/tools/rctl.py" add-route 10.0.2.0/24 rt2 >/dev/null 2>&1
sleep 0.5
ip netns exec h1 ping -c 2 -W 2 10.0.2.10 >/dev/null 2>&1
check $? "traffic resumes without restarting the router"

echo
if [[ $FAILURES -eq 0 ]]; then
    echo "integration tests passed"
    exit 0
fi
echo "$FAILURES integration test(s) failed"
cat /tmp/ci-router.log
exit 1
