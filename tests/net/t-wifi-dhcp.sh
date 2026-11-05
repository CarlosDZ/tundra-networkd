. "$(dirname "$0")/lib.sh"

wcli() {
	wpa_cli -p "$RUN/wpa" -i "$WIF" "$@"
}

ap_mac() {
	ip -n "$WNS" -o link show "$1" | sed -n 's/.*link\/ether \([0-9a-f:]*\).*/\1/p'
}

wifi_addr() {
	in_ns ip -4 -o addr show dev "$WIF" | awk '{print $4}'
}

current_bssid() {
	wcli status | sed -n 's/^bssid=//p'
}

bssid_is() {
	[ "$(current_bssid)" = "$1" ]
}

fresh_scan_has() {
	for _n in 1 2 3 4 5 6 7 8 9 10; do
		wcli scan >/dev/null
		sleep 1
		wcli scan_results | grep -q "^$1	.*	$2\$" && return 0
	done
	return 1
}

roam_to() {
	_r=$(wcli roam "$1")
	[ "$_r" = OK ] && return 0
	echo "# roam $1: $_r"
	wcli status | sed 's/^/#   /'
	wcli scan_results | sed 's/^/#   /'
	return 1
}

dhcpc_pid() {
	sed -n "s/.*$WIF: tundra-dhcpc started for profile $1 (pid \([0-9]*\)).*/\1/p" "$LOG" |
		tail -n 1
}

args_have() {
	tr '\0' ' ' <"/proc/$1/cmdline" | grep -q -- "$2"
}

count_log() {
	grep -c -- "$1" "$LOG"
}

status_matches() {
	cmd STATUS | grep -qx -- "$1"
}

profiles_have() {
	"$TOOLS/ctl" "$SOCK" PROFILES | grep -qx -- "$1"
}

if ! wifi_ready; then
	not_ok "WiFi prerequisites: hostapd, iw, wpa_cli, $WPA_REAL and mac80211_hwsim"
	summary
	exit
fi
if [ ! -x "$DHCPC_REAL" ]; then
	not_ok "tundra-dhcpc test build at $DHCPC_REAL"
	summary
	exit
fi

setup
if ! wifi_setup; then
	not_ok "three mac80211_hwsim radios in their namespaces"
	summary
	exit
fi
physical "$WIF"
mkdir -p "$SYS/$WIF/phy80211"
wifi_profile 10-casa tnd-wpa3 wpa3 "correct horse wpa3"
wifi_profile 20-cafe "tnd open" open "" no
start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3"
start_ap "$WAP2" "tnd open" open
AP1=$(ap_mac "$WAP1")
AP2=$(ap_mac "$WAP2")

start_daemon
expect_log "dhcpc\[$WIF/10-casa\]: bound to $WNET\." 30 "the WPA3 autoconnect profile associates and gets a lease"
check "tundra-dhcpc has lease-key 10-casa and carrier-grace 3000" args_have "$(dhcpc_pid 10-casa)" "carrier-grace 3000 lease-key 10-casa"
eventually "STATUS shows the ssid and the bssid" 5 status_matches "iface $WIF state connected profile 10-casa address $WNET\.[0-9]*/24 trusted no override - alert - ssid tnd-wpa3 bssid $AP1"
check "PROFILES shows ssid and security" profiles_have "profile 10-casa interface $WIF autoconnect yes trusted no ssid tnd-wpa3 security wpa3"
check "with the ssid encoded" profiles_have "profile 20-cafe interface $WIF autoconnect no trusted no ssid tnd%20open security open"

check "CONNECT to another SSID" reply_is "OK" CONNECT 20-cafe
expect_log "dhcpc\[$WIF/10-casa\]: releasing address on $WIF" 15 "the previous tundra-dhcpc gets SIGUSR1"
expect_log "dhcpc\[$WIF/20-cafe\]: bound to $WNET\." 30 "and the other profile gets a lease"
check "under its own lease-key" test -e "$LEASES/$WIF-20-cafe.lease"
eventually "STATUS follows the new network" 5 status_matches "iface $WIF state connected profile 20-cafe address $WNET\.[0-9]*/24 trusted no override - alert - ssid tnd%20open bssid $AP2"

stop_ap "$WAP2"
check "a second access point with the same SSID" start_ap "$WAP2" tnd-wpa3 wpa3 "correct horse wpa3"
check "CONNECT back to 10-casa" reply_is "OK" CONNECT 10-casa
eventually "it is connected again" 30 status_matches "iface $WIF state connected profile 10-casa address $WNET\.[0-9]*/24 trusted no override - alert - ssid tnd-wpa3 bssid .*"
eventually "with its address on the interface" 10 sh -c "ip netns exec '$NS' ip -4 -o addr show dev '$WIF' | grep -q ' $WNET\.'"
pid=$(dhcpc_pid 10-casa)
addr=$(wifi_addr)
if bssid_is "$AP1"; then to=$AP2; else to=$AP1; fi
check "a fresh scan sees the other access point with the same SSID" fresh_scan_has "$to" tnd-wpa3
releases=$(count_log "dhcpc\[$WIF/10-casa\]: releasing")
starts=$(count_log "$WIF: tundra-dhcpc started")
if roam_to "$to"; then ok "roam to the other one"; else not_ok "roam to the other one"; fi
eventually "the station roams" 15 bssid_is "$to"
eventually "STATUS shows the new bssid" 5 status_matches "iface $WIF state connected profile 10-casa address $addr trusted no override - alert - ssid tnd-wpa3 bssid $to"
sleep 4
check "tundra-dhcpc keeps running" kill -0 "$pid"
check "without a SIGUSR1" test "$(count_log "dhcpc\[$WIF/10-casa\]: releasing")" -eq "$releases"
check "nor another tundra-dhcpc" test "$(count_log "$WIF: tundra-dhcpc started")" -eq "$starts"
check "and keeps its address" test "$(wifi_addr)" = "$addr"

cable_profile 90-any "*"
check "RELOAD with a profile without ssid" reply_is "OK" RELOAD
stop_ap "$WAP2"
check "an access point without a profile" start_ap "$WAP2" "tnd manual" open
id=$(wcli add_network | tail -n 1)
wcli set_network "$id" ssid '"tnd manual"' >/dev/null
wcli set_network "$id" key_mgmt NONE >/dev/null
check "associating to it by hand" sh -c "wpa_cli -p '$RUN/wpa' -i '$WIF' select_network $id | grep -qx OK"
expect_log "dhcpc\[$WIF/90-any\]: bound to $WNET\." 30 "applies the first profile without ssid"
stop_daemon
summary
