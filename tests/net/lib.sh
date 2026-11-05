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
SOCK=/run/tundra-test/network.socket
TIP_FAIL=$WORK/tip-fail
export TIP_FAIL
DHCPC_FAKE_DIR=$WORK/fake
DHCPC_REAL=${DHCPC_REAL:-$ROOT/../tundra-dhcpc/build/test/tundra-dhcpc}
BIARP_FAKE_DIR=$WORK/biarp
BIARP_REAL=${BIARP_REAL:-$ROOT/../tundra-blueice-arp/build/test/tundra-blueice-arp}
ARPSEND=${ARPSEND:-$ROOT/../tundra-blueice-arp/build/test/tools/arpsend}
export DHCPC_FAKE_DIR DHCPC_REAL BIARP_FAKE_DIR BIARP_REAL
WNS=tndap
WIF=tndw0
WAP1=tndap0
WAP2=tndap1
WBR=tndapbr
WNET=10.80.0
WPA_REAL=${WPA_REAL:-/usr/bin/wpa_supplicant}
export WPA_REAL

PASS=0
FAIL=0
DNSMASQ_PID=
DAEMON_PID=
DAEMON_EXIT=
EV_PID=

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

no_addr() {
	[ -z "$(ns_addr)" ]
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

sensor() {
	mkdir -p "$BIARP_FAKE_DIR"
	echo "$1" >"$BIARP_FAKE_DIR/mode"
}

probe() {
	mkdir -p "$BIARP_FAKE_DIR"
	echo "$1" >"$BIARP_FAKE_DIR/probe"
}

probes() {
	cat "$BIARP_FAKE_DIR/probes" 2>/dev/null | wc -l
}

static_profile() {
	write_conf "$1.tnc" <<END
match {
	interface = $IF
}
apply {
	address = $2
	gateway = $NET.1
	dns = ${3:-9.9.9.9}
	autoconnect = yes
	trusted = no
}
END
}

sensor_launches() {
	cat "$BIARP_FAKE_DIR/launches" 2>/dev/null | wc -l
}

sensor_launched() {
	[ "$(sensor_launches)" -ge "$1" ]
}

last_sensor() {
	tail -n 1 "$BIARP_FAKE_DIR/launches" 2>/dev/null
}

dhcpc_got() {
	grep -q "^$1 $2\$" "$DHCPC_FAKE_DIR/signals" 2>/dev/null
}

sensor_got() {
	grep -q "^$1 $2\$" "$BIARP_FAKE_DIR/signals" 2>/dev/null
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

cmd() {
	"$TOOLS/ctl" "$SOCK" "$*"
}

reply_is() {
	_want=$1
	shift
	[ "$(cmd "$@")" = "$_want" ]
}

status_has() {
	cmd STATUS | grep -qx -- "$1"
}

subscribe() {
	"$TOOLS/ctl" -f "$SOCK" SUBSCRIBE >"$WORK/events" 2>&1 &
	EV_PID=$!
}

unsubscribe() {
	[ -n "$EV_PID" ] || return 0
	kill "$EV_PID" 2>/dev/null
	wait "$EV_PID" 2>/dev/null
	EV_PID=
}

has_event() {
	grep -qx -- "$1" "$WORK/events"
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
	{
		printf 'address=%s\nmask=255.255.255.0\n' "${3:-$NET.200}"
		[ -n "${4:-}" ] && printf 'router=%s\n' "$4"
		printf 'dns=%s\nlease=3600\n' "$2"
	} >"$LEASES/$1.lease.tmp"
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

wifi_ready() {
	for c in hostapd iw wpa_cli "$WPA_REAL"; do
		command -v "$c" >/dev/null || return 1
	done
	modprobe -n mac80211_hwsim 2>/dev/null
}

# Radios made by netlink belong to the creator's netns and die with it. The
# module's own start in the host netns and return there, where a wireless
# manager can take them and block the module removal for good.
wifi_radio() {
	ip netns exec "$WNS" "$TOOLS/hwsim-radio" >/dev/null || return 1
	ip -n "$WNS" link set wlan0 name "$1"
}

wifi_setup() {
	modprobe mac80211_hwsim radios=0 || return 1
	ip netns add "$WNS"
	ip -n "$WNS" link set lo up
	wifi_radio "$WAP1" || return 1
	wifi_radio "$WAP2" || return 1
	wifi_radio "$WIF" || return 1
	_phy=$(ip netns exec "$WNS" cat "/sys/class/net/$WIF/phy80211/name") ||
		return 1
	ip netns exec "$WNS" iw phy "$_phy" set netns name "$NS" || return 1
	ip -n "$WNS" link add "$WBR" type bridge
	ip -n "$WNS" addr add "$WNET.1/24" dev "$WBR"
	ip -n "$WNS" link set "$WBR" up
	ip netns exec "$WNS" dnsmasq --interface="$WBR" --bind-interfaces \
		--port=0 --no-ping --dhcp-range="$WNET.100,$WNET.150,2m" \
		--dhcp-option=3,"$WNET.1" --dhcp-leasefile="$WORK/wleases" \
		--pid-file="$WORK/wdnsmasq.pid"
}

# The module stays loaded: reloading it can leave /sys/devices/virtual/
# mac80211_hwsim behind, and then no radio can be created until a reboot.
wifi_teardown() {
	kill_ns "$WNS"
	rm -f "$LEASES/$WIF".lease "$LEASES/$WIF"-*.lease
}

start_ap() {
	_conf=$WORK/hostapd-$1.conf
	{
		echo "interface=$1"
		echo "bridge=$WBR"
		echo "driver=nl80211"
		echo "ssid=$2"
		echo "hw_mode=g"
		echo "channel=${5:-1}"
		case $3 in
		wpa2)
			echo "wpa=2"
			echo "wpa_key_mgmt=WPA-PSK"
			echo "rsn_pairwise=CCMP"
			echo "wpa_passphrase=$4"
			echo "ieee80211w=1"
			;;
		wpa3)
			echo "wpa=2"
			echo "wpa_key_mgmt=SAE"
			echo "rsn_pairwise=CCMP"
			echo "sae_password=$4"
			echo "sae_pwe=2"
			echo "ieee80211w=2"
			;;
		esac
	} >"$_conf"
	ip netns exec "$WNS" hostapd -t "$_conf" >"$WORK/hostapd-$1.log" 2>&1 &
	echo $! >"$WORK/hostapd-$1.pid"
	n=50
	while ! grep -q AP-ENABLED "$WORK/hostapd-$1.log"; do
		[ "$n" -le 0 ] && return 1
		sleep 0.1
		n=$((n - 1))
	done
}

stop_ap() {
	[ -e "$WORK/hostapd-$1.pid" ] || return 0
	kill "$(cat "$WORK/hostapd-$1.pid")" 2>/dev/null
	rm -f "$WORK/hostapd-$1.pid"
	sleep 0.2
}

teardown_net() {
	unsubscribe
	kill_daemon
	stop_dnsmasq
	kill_ns "$NS"
	kill_ns "$EVIL"
	wifi_teardown
	ip link del "${IF}p" 2>/dev/null
	ip link del "${IF2}p" 2>/dev/null
	ip link del "${EVIF}p" 2>/dev/null
	ip link del "$BR" 2>/dev/null
	rm -rf "$CONF" "$RUN" "$SYS" "$DHCPC_FAKE_DIR" "$BIARP_FAKE_DIR" \
		"$(dirname "$SOCK")"
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
