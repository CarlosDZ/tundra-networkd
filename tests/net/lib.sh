set -u

NS=tnd
EVIL=tndevil
BR=tndbr0
IF=tnd0
IF2=tnd1
EVIF=tnde0
NET=10.79.0
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NETWORKD=$ROOT/build/test/tundra-networkd
TOOLS=$ROOT/build/test/tools
CONF=$ROOT/build/test/conf
RUN=$ROOT/build/test/run
SYS=$ROOT/build/test/sys
WORK=$(mktemp -d /tmp/tnd-test.XXXXXX)
LOG=$WORK/networkd.log
LEASES=/run/tundra/dhcpc
SENSOR_HISTORY=/run/tundra/blueice/arp
DHCPC_FAKE_DIR=$WORK/fake
DHCPC_REAL=${DHCPC_REAL:-$ROOT/../tundra-dhcpc/build/test/tundra-dhcpc}
export DHCPC_FAKE_DIR DHCPC_REAL

PASS=0
FAIL=0
DNSMASQ_PID=
DAEMON_PID=
DAEMON_EXIT=

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

has_addr() {
	[ -n "$(ns_addr)" ]
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

write_conf() {
	(umask 077 && cat >"$CONF/$1")
}

cable_profile() {
	write_conf "$1.tnc" <<END
match {
	interface = ${2:-$IF}
}
apply {
	address = dhcp
	dns = ${3:-lease}
	autoconnect = yes
	trusted = no
}
END
}

physical() {
	mkdir -p "$SYS/$1/device"
}

fake() {
	mkdir -p "$DHCPC_FAKE_DIR"
	echo "$1" >"$DHCPC_FAKE_DIR/mode"
}

launches() {
	cat "$DHCPC_FAKE_DIR/launches" 2>/dev/null | wc -l
}

launched() {
	[ "$(launches)" -ge "$1" ]
}

start_daemon() {
	: >"$LOG"
	ip netns exec "$NS" "$NETWORKD" >>"$LOG" 2>&1 &
	DAEMON_PID=$!
}

wait_daemon() {
	n=$(($1 * 10))
	while kill -0 "$DAEMON_PID" 2>/dev/null; do
		[ "$n" -le 0 ] && return 1
		sleep 0.1
		n=$((n - 1))
	done
	wait "$DAEMON_PID" 2>/dev/null
	DAEMON_EXIT=$?
	DAEMON_PID=
	return 0
}

stop_daemon() {
	[ -n "$DAEMON_PID" ] || return 0
	kill -TERM "$DAEMON_PID" 2>/dev/null
	wait_daemon 10 || kill_daemon
}

kill_daemon() {
	[ -n "$DAEMON_PID" ] || return 0
	kill -KILL "$DAEMON_PID" 2>/dev/null
	wait "$DAEMON_PID" 2>/dev/null
	DAEMON_EXIT=$?
	DAEMON_PID=
}

expect_exit() {
	if [ "$DAEMON_EXIT" = "$1" ]; then
		ok "$2"
	else
		not_ok "$2 (exit $DAEMON_EXIT)"
	fi
}

child_pid() {
	sed -n "s/.*tundra-dhcpc started for profile $1 (pid \([0-9]*\)).*/\1/p" \
		"$LOG" | tail -n 1
}

add_if() {
	_if=${1:-$IF}
	ip link add "$_if" type veth peer name "${_if}p"
	ip link set "$_if" netns "$NS"
	ip link set "${_if}p" master "$BR" up
	in_ns ip link set "$_if" up
}

write_lease() {
	mkdir -p "$LEASES"
	printf 'address=%s\nmask=255.255.255.0\ndns=%s\nlease=3600\n' \
		"$NET.200" "$2" >"$LEASES/$1.lease.tmp"
	mv "$LEASES/$1.lease.tmp" "$LEASES/$1.lease"
}

resolv_is() {
	[ -e "$RUN/resolv.conf" ] && [ "$(cat "$RUN/resolv.conf")" = "$1" ]
}

kill_ns() {
	for pid in $(ip netns pids "$1" 2>/dev/null); do
		kill "$pid" 2>/dev/null
	done
	ip netns del "$1" 2>/dev/null
}

teardown_net() {
	kill_daemon
	stop_dnsmasq
	kill_ns "$NS"
	kill_ns "$EVIL"
	ip link del "${IF}p" 2>/dev/null
	ip link del "${IF2}p" 2>/dev/null
	ip link del "${EVIF}p" 2>/dev/null
	ip link del "$BR" 2>/dev/null
	rm -rf "$CONF" "$RUN" "$SYS" "$DHCPC_FAKE_DIR"
	rm -f "$LEASES/$IF".lease "$LEASES/$IF"-*.lease "$LEASES/$IF2"-*.lease
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
	in_ns ip link set lo up
	add_if
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
