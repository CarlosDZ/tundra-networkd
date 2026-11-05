. "$(dirname "$0")/lib.sh"

wpa_pid() {
	sed -n "s/.*$WIF: wpa_supplicant started (pid \([0-9]*\)).*/\1/p" "$LOG" |
		tail -n 1
}

logged_times() {
	[ "$(grep -c -- "$1" "$LOG")" -ge "$2" ]
}

wpa_conf_is() {
	[ "$(cat "$RUN/wpa/$WIF.conf")" = "$(printf 'ctrl_interface=%s\nupdate_config=0' "$RUN/wpa")" ]
}

wpa_ping() {
	[ "$(wpa_cli -p "$RUN/wpa" -i "$WIF" ping)" = PONG ]
}

no_wpa() {
	! pgrep -f "wpa_supplicant -i $WIF " >/dev/null
}

wifi_down() {
	! in_ns ip -o link show "$WIF" | grep -q '[<,]UP[,>]'
}

down_again() {
	logged_times "$WIF: wifi is off, bringing the interface down" 2 && wifi_down
}

quiet_for() {
	sleep "$1"
	! grep -q "$WIF: wpa_supplicant started" "$LOG"
}

if ! wifi_ready; then
	not_ok "WiFi prerequisites: hostapd, iw, wpa_cli, $WPA_REAL and mac80211_hwsim"
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

start_daemon
expect_log "$WIF: wpa_supplicant started (pid" 5 "the wireless interface gets its wpa_supplicant"
check "with the minimal .conf in run/wpa" wpa_conf_is
check "readable only by root" test "$(stat -c %a "$RUN/wpa/$WIF.conf")" = 600
expect_log "$WIF: wpa_supplicant ready" 10 "networkd attaches to its control socket"
check "which answers PING" wpa_ping
expect_log "^wpa\[$WIF\]: " 5 "its output reaches the log with the wpa[$WIF] prefix"

kill -9 "$(wpa_pid)"
expect_log "$WIF: wpa_supplicant killed by signal 9" 3 "kill -9 is noticed"
expect_log "$WIF: relaunching wpa_supplicant in 1000 ms" 1 "and it is relaunched with backoff"
eventually "a new one starts" 5 logged_times "$WIF: wpa_supplicant started" 2
eventually "and is attached again" 10 logged_times "$WIF: wpa_supplicant ready" 2
check "answering PING" wpa_ping

cable_profile casa "$WIF"
write_conf interfaces <<END
$WIF {
	wifi = no
}
END
check "RELOAD with wifi = no" reply_is "OK" RELOAD
eventually "wpa_supplicant stops" 7 no_wpa
eventually "the interface goes down" 3 wifi_down
check "STATUS says off" status_has "iface $WIF state off profile - address - trusted - override - alert -"
check "an autoconnect profile does not connect" test "$(launches)" -eq 0
check "CONNECT says the wifi is off" reply_is "ERROR wifi is off" CONNECT casa
in_ns ip link set "$WIF" up
eventually "brought up by hand, it goes down again" 3 down_again

stop_daemon
start_daemon
wait_log "control socket on" 3
check "a daemon started with wifi = no launches no wpa_supplicant" quiet_for 1
check "and the interface stays down" wifi_down
stop_daemon
summary
