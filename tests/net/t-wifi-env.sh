. "$(dirname "$0")/lib.sh"

sta_start() {
	cat >"$WORK/sta.conf"
	ip netns exec "$NS" "$WPA_REAL" -i "$WIF" -D nl80211 -c "$WORK/sta.conf" \
		>"$WORK/sta.log" 2>&1 &
	echo $! >"$WORK/sta.pid"
}

sta_stop() {
	[ -e "$WORK/sta.pid" ] && kill "$(cat "$WORK/sta.pid")" 2>/dev/null
	rm -f "$WORK/sta.pid"
	sleep 0.3
}

sta_completed() {
	wpa_cli -p "$WORK/stactl" -i "$WIF" status | grep -qx wpa_state=COMPLETED
}

wifi_addr() {
	in_ns ip -4 -o addr show dev "$WIF" | grep -q " $WNET\."
}

lease_over_wifi() {
	ip netns exec "$NS" "$DHCPC_REAL" "$WIF" probe off >"$WORK/dhcpc.log" 2>&1 &
	_pid=$!
	eventually "$1: the station gets a lease from the access point" 20 wifi_addr
	kill "$_pid" 2>/dev/null
	wait "$_pid" 2>/dev/null
	in_ns ip -4 addr flush dev "$WIF"
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
ok "three mac80211_hwsim radios in their namespaces"
check "the station is wireless" in_ns test -e "/sys/class/net/$WIF/phy80211"
check "and has a device, so it counts as physical" in_ns test -e "/sys/class/net/$WIF/device"

check "an open access point" start_ap "$WAP1" "tnd open" open
sta_start <<END
ctrl_interface=$WORK/stactl
network={
	ssid="tnd open"
	key_mgmt=NONE
}
END
eventually "the station associates without security" 15 sta_completed
lease_over_wifi open
sta_stop
stop_ap "$WAP1"

check "a WPA2 access point" start_ap "$WAP1" tnd-wpa2 wpa2 "correct horse wpa2"
sta_start <<END
ctrl_interface=$WORK/stactl
network={
	ssid="tnd-wpa2"
	key_mgmt=WPA-PSK
	psk="correct horse wpa2"
	ieee80211w=1
}
END
eventually "the station associates with WPA2" 15 sta_completed
lease_over_wifi wpa2
sta_stop
stop_ap "$WAP1"

check "a WPA3 access point" start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3"
sta_start <<END
ctrl_interface=$WORK/stactl
sae_pwe=2
network={
	ssid="tnd-wpa3"
	key_mgmt=SAE
	sae_password="correct horse wpa3"
	ieee80211w=2
}
END
eventually "the station associates with WPA3 (SAE)" 15 sta_completed
check "with SAE as key management" sh -c "wpa_cli -p '$WORK/stactl' -i '$WIF' status | grep -qx key_mgmt=SAE"
lease_over_wifi wpa3
sta_stop
stop_ap "$WAP1"
summary
