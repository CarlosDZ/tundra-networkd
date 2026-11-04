set -u

NS=tnd
EVIL=tndevil
BR=tndbr0
IF=tnd0
EVIF=tnde0
NET=10.79.0
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NETWORKD=$ROOT/build/test/tundra-networkd
TOOLS=$ROOT/build/test/tools
CONF=$ROOT/build/test/conf
RUN=$ROOT/build/test/run
WORK=$(mktemp -d /tmp/tnd-test.XXXXXX)
LOG=$WORK/networkd.log
LEASES=/run/tundra/dhcpc
SENSOR_HISTORY=/run/tundra/blueice/arp

PASS=0
FAIL=0
DNSMASQ_PID=

ok() {
	PASS=$((PASS + 1))
	printf 'ok - %s\n' "$1"
}

not_ok() {
	FAIL=$((FAIL + 1))
	printf 'not ok - %s\n' "$1"
}

check() {
	desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		ok "$desc"
	else
		not_ok "$desc"
	fi
}

eventually() {
	desc=$1
	n=$(($2 * 10))
	shift 2
	while [ "$n" -gt 0 ]; do
		if "$@" >/dev/null 2>&1; then
			ok "$desc"
			return 0
		fi
		sleep 0.1
		n=$((n - 1))
	done
	not_ok "$desc"
}

wait_log() {
	n=$(($2 * 10))
	while [ "$n" -gt 0 ]; do
		grep -q -- "$1" "$LOG" 2>/dev/null && return 0
		sleep 0.1
		n=$((n - 1))
	done
	return 1
}

expect_log() {
	if wait_log "$1" "$2"; then
		ok "$3"
	else
		not_ok "$3"
		sed 's/^/#   /' "$LOG"
	fi
}

in_ns() {
	ip netns exec "$NS" "$@"
}

in_evil() {
	ip netns exec "$EVIL" "$@"
}

ns_addr() {
	in_ns ip -4 -o addr show dev "$IF" | awk '{print $4}' | cut -d/ -f1
}

start_dnsmasq() {
	dnsmasq --interface="$BR" --bind-interfaces --port=0 --no-ping \
		--dhcp-range="${DHCP_RANGE:-$NET.100,$NET.150,2m}" \
		--dhcp-option=3,"${DHCP_ROUTER:-$NET.1}" \
		--dhcp-leasefile="$WORK/leases" --pid-file="$WORK/dnsmasq.pid" "$@"
	DNSMASQ_PID=$(cat "$WORK/dnsmasq.pid")
}

stop_dnsmasq() {
	if [ -n "$DNSMASQ_PID" ]; then
		kill "$DNSMASQ_PID" 2>/dev/null
		DNSMASQ_PID=
		sleep 0.2
	fi
}

kill_ns() {
	for pid in $(ip netns pids "$1" 2>/dev/null); do
		kill "$pid" 2>/dev/null
	done
	ip netns del "$1" 2>/dev/null
}

teardown_net() {
	stop_dnsmasq
	kill_ns "$NS"
	kill_ns "$EVIL"
	ip link del "${IF}p" 2>/dev/null
	ip link del "${EVIF}p" 2>/dev/null
	ip link del "$BR" 2>/dev/null
	rm -rf "$CONF" "$RUN"
	rm -f "$LEASES/$IF".lease "$LEASES/$IF"-*.lease
	rm -f "$SENSOR_HISTORY/$IF" "$SENSOR_HISTORY/$IF.lock"
	return 0
}

setup() {
	teardown_net
	mkdir -p "$CONF" "$RUN"
	ip link add "$BR" type bridge
	ip addr add "$NET.1/24" dev "$BR"
	ip link set "$BR" up
	ip netns add "$NS"
	ip link add "$IF" type veth peer name "${IF}p"
	ip link set "$IF" netns "$NS"
	ip link set "${IF}p" master "$BR" up
	in_ns ip link set lo up
	in_ns ip link set "$IF" up
	ip netns add "$EVIL"
	ip link add "$EVIF" type veth peer name "${EVIF}p"
	ip link set "$EVIF" netns "$EVIL"
	ip link set "${EVIF}p" master "$BR" up
	in_evil ip link set "$EVIF" up
	start_dnsmasq
}

teardown() {
	teardown_net
	rm -rf "$WORK"
}

summary() {
	printf '# %d passed, %d failed\n' "$PASS" "$FAIL"
	[ "$FAIL" -eq 0 ]
}

trap teardown EXIT
trap 'exit 130' INT TERM
