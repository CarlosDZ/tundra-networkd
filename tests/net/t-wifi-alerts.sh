. "$(dirname "$0")/lib.sh"

wcli() {
	wpa_cli -p "$RUN/wpa" -i "$WIF" "$@"
}

net_is() {
	for _i in $(wcli list_networks | tail -n +2 | cut -f 1); do
		if [ "$(wcli get_network "$_i" id_str)" = "\"$1\"" ]; then
			[ "$(wcli get_network "$_i" "$2")" = "$3" ]
			return
		fi
	done
	return 1
}

on_profile() {
	wcli status | grep -qx "wpa_state=COMPLETED" &&
		wcli status | grep -qx "id_str=$1"
}

wifi_ip() {
	in_ns ip -4 -o addr show dev "$WIF" | awk '{print $4}' | cut -d/ -f1
}

status_matches() {
	cmd STATUS | grep -qx -- "$1"
}

sensor_accepts() {
	_pid=$(sed -n "s/.*$WIF: tundra-blueice-arp watching .* (pid \([0-9]*\)).*/\1/p" "$LOG" |
		tail -n 1)
	[ -n "$_pid" ] && tr '\0' ' ' <"/proc/$_pid/cmdline" | grep -q " accept $1"
}

if ! wifi_ready; then
	not_ok "WiFi prerequisites: hostapd, iw, wpa_cli, $WPA_REAL and mac80211_hwsim"
	summary
	exit
fi
for bin in "$DHCPC_REAL" "$BIARP_REAL" "$ARPSEND"; do
	if [ ! -x "$bin" ]; then
		not_ok "test build at $bin"
		summary
		exit
	fi
done

setup
if ! wifi_setup; then
	not_ok "three mac80211_hwsim radios in their namespaces"
	summary
	exit
fi
physical "$WIF"
mkdir -p "$SYS/$WIF/phy80211"
wifi_profile 10-casa tnd-wpa3 wpa3 "correct horse wpa3"
wifi_profile 20-cafe "tnd open" open ""
start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3"
start_ap "$WAP2" "tnd open" open

start_daemon
expect_log "dhcpc\[$WIF/10-casa\]: bound to $WNET\." 30 "the first profile gets a lease"
expect_log "blueice-arp\[$WIF\]: gateway is at " 15 "the sensor watches the WiFi lease"
ip netns exec "$WNS" "$ARPSEND" "$WBR" 02:bb:bb:bb:bb:bb "$WNET.1" "$(wifi_ip)"
expect_log "blueice-arp\[$WIF\]: alert 111/8 stop by tundra-blueice-arp on $WIF .* case D " 5 "a second MAC for the gateway raises D"
expect_log "$WIF: network of profile 10-casa disabled by an alert of level 8" 3 "the stop disables that network"
eventually "in wpa_supplicant" 5 net_is 10-casa disabled 1
eventually "another profile of the interface associates" 20 on_profile 20-cafe
expect_log "dhcpc\[$WIF/20-cafe\]: bound to $WNET\." 30 "and gets its lease"
eventually "STATUS shows it with the pending alert" 5 status_matches "iface $WIF state connected profile 20-cafe address $WNET\.[0-9]*/24 trusted no override - alert 1 ssid tnd%20open bssid .*"
check "CONNECT to the stopped network waits for the answer" reply_is "ERROR alert pending, answer it first" CONNECT 10-casa

check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "goes back to that network" 20 on_profile 10-casa
eventually "connected again, still untrusted" 30 status_matches "iface $WIF state connected profile 10-casa address $WNET\.[0-9]*/24 trusted no override 1 alert - ssid tnd-wpa3 bssid .*"
eventually "with the sensor accepting D up to level 8" 15 sensor_accepts D/8
stop_daemon

cat >"$WORK/wpa-alert.sh" <<END
#!/bin/sh
echo "alert 111/9 stop by tundra-blueice-arp on $WIF ts 0.000 reply force,disconnect info case D -- through the wpa_supplicant pipe"
exec sleep 30
END
chmod 755 "$WORK/wpa-alert.sh"
real_wpa=$WPA_REAL
WPA_REAL=$WORK/wpa-alert.sh
start_daemon
expect_log "wpa\[$WIF\]: alert 111/9 stop by tundra-blueice-arp" 5 "a line like an alert on the wpa_supplicant pipe reaches the log"
sleep 0.5
check "but is not taken as one" sh -c "! grep -q 'alert [0-9]* recorded' '$LOG'"
check "ALERTS stays empty" reply_is "OK" ALERTS
stop_daemon
WPA_REAL=$real_wpa
summary
