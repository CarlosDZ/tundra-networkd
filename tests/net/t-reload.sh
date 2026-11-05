. "$(dirname "$0")/lib.sh"

ifaces() {
	write_conf interfaces <<END
$IF {
	$1
}
END
}

untouched() {
	sleep 0.5
	[ "$(launches)" -eq "$1" ] && ! grep -q "^$2 " "$DHCPC_FAKE_DIR/signals" 2>/dev/null
}

setup
ifaces "metric = 100"
cable_profile casa
fake run
sensor run
start_daemon
eventually "casa connects" 5 launched 1
wait_log "control socket on" 3
write_lease "$IF-casa" "10.0.0.1"
eventually "with the servers of its lease" 3 resolv_is "nameserver 10.0.0.1"

pid=$(child_pid casa)
check "RELOAD answers OK" reply_is "OK" RELOAD
check "a reload without changes touches nothing" untouched 1 "$pid"

cable_profile casa "$IF" "10.0.0.9"
check "RELOAD with a new dns" reply_is "OK" RELOAD
eventually "only dns: resolv.conf is regenerated" 3 resolv_is "nameserver 10.0.0.9"
check "and the connection is not touched" untouched 1 "$pid"

ifaces "metric = 300"
check "RELOAD with a new metric" reply_is "OK" RELOAD
eventually "any other change releases the connection" 3 grep -q "^$pid USR1\$" "$DHCPC_FAKE_DIR/signals"
eventually "and connects it again" 3 launched 2
check "with the new metric" sh -c "tail -n 1 '$DHCPC_FAKE_DIR/launches' | grep -q ' metric 300 '"

pid=$(child_pid casa)
write_conf casa.tnc <<END
match {
	interface = $IF
}
END
check "RELOAD with an error in the profile" reply_is "OK" RELOAD
check "the connection keeps the previous configuration" untouched 2 "$pid"
expect_log "$IF: profile casa has errors, keeping the previous configuration" 1 "and says so"
check "STATUS shows it" status_has "iface $IF state connected profile casa address $NET.200/24 trusted no override - alert - ssid - bssid - -- previous configuration kept: profile casa has errors"
check "PROFILES shows the error" sh -c "$TOOLS/ctl '$SOCK' PROFILES | grep -q '^profile casa invalid -- '"
check "the profile cannot be connected again" reply_is "ERROR profile has errors" CONNECT casa

cable_profile casa "$IF" "10.0.0.9"
ifaces "metric = x"
check "RELOAD with an error in interfaces" reply_is "OK" RELOAD
check "the connection is kept too" untouched 2 "$pid"
expect_log "$IF: interfaces has errors, keeping the previous configuration" 1 "and says so"
expect_log "networkd: interfaces: $IF: " 1 "with the error of the block"

ifaces "metric = 300"
check "RELOAD once fixed" reply_is "OK" RELOAD
check "with the same configuration nothing changes" untouched 2 "$pid"
check "and STATUS drops the note" status_has "iface $IF state connected profile casa address $NET.200/24 trusted no override - alert - ssid - bssid -"

mv "$CONF" "$CONF.away"
check "RELOAD without a configuration directory" reply_is "ERROR cannot read the configuration" RELOAD
mv "$CONF.away" "$CONF"
check "keeps everything" untouched 2 "$pid"

subscribe
eventually "subscribed" 3 has_event OK
rm -f "$CONF/casa.tnc"
kill -HUP "$DAEMON_PID"
eventually "SIGHUP reloads: a deleted profile disconnects" 3 grep -q "^$pid USR1\$" "$DHCPC_FAKE_DIR/signals"
eventually "with event disconnected" 3 has_event "event disconnected $IF casa"
check "and nothing takes its place" untouched 2 none
check "STATUS shows it idle" status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"

ifaces "wifi = no"
check "RELOAD with wifi on a cable" reply_is "OK" RELOAD
check "leaves the cable unmanaged, with the reason" status_has "iface $IF state unmanaged profile - address - trusted - override - alert - ssid - bssid - -- interfaces: line 2: wifi on a wired interface"
ifaces "metric = 300"

cable_profile casa
check "RELOAD with a new profile" reply_is "OK" RELOAD
eventually "it autoconnects" 3 launched 3

fake exit2
check "DISCONNECT" reply_is "OK" DISCONNECT "$IF"
check "CONNECT with a dhcpc that rejects it" reply_is "OK" CONNECT casa
eventually "the profile is blocked" 3 status_has "iface $IF state blocked reason config profile casa address - trusted no override - alert - ssid - bssid -"
check "CONNECT asks for a reload" reply_is "ERROR profile rejected by tundra-dhcpc, reload first" CONNECT casa
fake run
sensor run
check "RELOAD" reply_is "OK" RELOAD
eventually "unblocks it" 3 launched 5
stop_daemon
summary
