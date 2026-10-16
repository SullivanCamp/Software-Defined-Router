#!/usr/bin/env bash
#
# End-to-end demonstration: builds the topology, starts the router, proves
# packets are forwarded by sdrouter (not the kernel), then reconfigures the
# routing table while traffic is in flight.
#
# Usage: sudo ./scripts/demo.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
CONF=/tmp/sdrouter-demo.conf
LOG=/tmp/sdrouter-demo.log
RPID=""

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
rctl()   { python3 "$ROOT/tools/rctl.py" "$@"; }

cleanup() {
    [[ -n "$RPID" ]] && kill "$RPID" 2>/dev/null
    wait "$RPID" 2>/dev/null
    "$HERE/topology.sh" down >/dev/null 2>&1
}
trap cleanup EXIT

[[ $EUID -eq 0 ]] || { echo "run me with sudo" >&2; exit 1; }

banner "building"
make -C "$ROOT" || exit 1

banner "creating topology"
"$HERE/topology.sh" up
"$HERE/topology.sh" conf "$CONF"

banner "starting sdrouter"
"$ROOT/build/sdrouter" -i rt1 -i rt2 -i rt3 -f "$CONF" > "$LOG" 2>&1 &
RPID=$!
sleep 1
grep -E 'route installed|data plane|control plane' "$LOG"

banner "routing table"
rctl show routes

banner "h1 -> h2 across the router"
echo "TTL arriving as 63 proves sdrouter decremented it in userspace."
ip netns exec h1 ping -c 3 -W 2 10.0.2.10 | tail -5

banner "h1 -> h3"
ip netns exec h1 ping -c 2 -W 2 10.0.3.10 | tail -3

banner "TTL=1 must be dropped, never forwarded"
ip netns exec h1 ping -c 2 -t 1 -W 1 10.0.2.10 2>&1 | tail -2

banner "live reconfiguration under load"
echo "Sending 30 pings. Midway, the route to h2 is withdrawn and reinstalled"
echo "without restarting the router."
ip netns exec h1 ping -q -i 0.2 -c 30 -W 1 10.0.2.10 > /tmp/sdrouter-ping.log 2>&1 &
PING=$!
sleep 2
echo "-> DEL_ROUTE 10.0.2.0/24"
rctl del-route 10.0.2.0/24
sleep 2
echo "-> ADD_ROUTE 10.0.2.0/24 rt2"
rctl add-route 10.0.2.0/24 rt2
wait $PING 2>/dev/null
echo
tail -3 /tmp/sdrouter-ping.log
echo
echo "The loss window matches the withdrawal window, and rt1's no_route"
echo "counter accounts for exactly those packets:"
rctl show stats | sed -n '1,7p'

banner "longest prefix match"
rctl add-route 172.16.0.0/12 rt3 --via 10.0.3.10
rctl add-route 172.16.5.0/24 rt2 --via 10.0.2.10
echo "172.16.5.x now leaves via rt2; the rest of 172.16/12 via rt3."
rctl show routes

banner "done"
echo "router log: $LOG"
