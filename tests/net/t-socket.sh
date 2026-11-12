. "$(dirname "$0")/lib.sh"

setup
add_if "$IF2"
cable_profile casa
cable_profile 999-dhcp '*'
write_conf manual.tnc <<END
match {
	interface = $IF2
}
apply {
	address = dhcp
	dns = lease
	autoconnect = no
	trusted = yes
}
END
write_conf roto.tnc <<END
match {
	interface = $IF
}
END
fake run
sensor run
start_daemon
expect_log "control socket on $SOCK" 5 "the control socket is up"
eventually "casa autoconnects" 5 launched 1

mode=$(stat -c '%a %U:%G' "$SOCK")
if getent group network >/dev/null; then
	check "the socket is 0660 root:network ($mode)" [ "$mode" = "660 root:network" ]
else
	check "without group network the socket is 0600 root:root ($mode)" [ "$mode" = "600 root:root" ]
	check "and the daemon says so" grep -q "group network not found, control socket limited to root" "$LOG"
fi
if command -v setpriv >/dev/null && setpriv --reuid=65534 --regid=65534 --clear-groups true 2>/dev/null; then
	cp "$TOOLS/ctl" "$(dirname "$SOCK")/ctl"
	check "a user outside the group cannot connect" sh -c "! setpriv --reuid=65534 --regid=65534 --clear-groups '$(dirname "$SOCK")/ctl' '$SOCK' ALERTS"
	if gid=$(getent group network | cut -d: -f3) && [ -n "$gid" ]; then
		check "a member of group network can connect" setpriv --reuid=65534 --regid=65534 --groups="$gid" "$(dirname "$SOCK")/ctl" "$SOCK" ALERTS
	fi
else
	echo "# skip: setpriv cannot switch users here"
fi

subscribe
eventually "SUBSCRIBE answers OK" 3 has_event OK
write_lease "$IF-casa" "10.0.0.1"
eventually "a lease emits event connected" 3 has_event "event connected $IF casa address $NET.200/24"
check "STATUS shows the connection" status_has "iface $IF state connected profile casa address $NET.200/24 trusted no override - alert - ssid - bssid -"
check "STATUS shows an idle interface" status_has "iface $IF2 state idle profile - address - trusted - override - alert - ssid - bssid -"
check "STATUS ends with OK" sh -c "$TOOLS/ctl '$SOCK' STATUS | tail -n 1 | grep -qx OK"
check "PROFILES lists the profiles in order" sh -c "$TOOLS/ctl '$SOCK' PROFILES | head -n 3 | tr '\n' '|' | grep -qx 'profile 999-dhcp interface \* autoconnect yes trusted no ssid - security -|profile casa interface $IF autoconnect yes trusted no ssid - security -|profile manual interface $IF2 autoconnect no trusted yes ssid - security -|'"
check "PROFILES shows the error of an invalid profile" sh -c "$TOOLS/ctl '$SOCK' PROFILES | grep -q '^profile roto invalid -- '"

check "CONNECT to an unknown profile" reply_is "ERROR unknown profile" CONNECT nope
check "CONNECT with the wildcard needs an interface" reply_is "ERROR interface required" CONNECT 999-dhcp
check "CONNECT on an interface the profile does not match" reply_is "ERROR profile does not match" CONNECT manual "$IF"
check "CONNECT with an invalid profile" reply_is "ERROR profile has errors" CONNECT roto
check "CONNECT on a missing interface" reply_is "ERROR no such interface" CONNECT casa tnd9
check "CONNECT without arguments" reply_is "ERROR usage: CONNECT <profile> [interface]" CONNECT
check "an unknown command" reply_is "ERROR unknown command" FOO
check "a command with extra arguments" reply_is "ERROR usage: STATUS" STATUS now
check "ALERTS is an empty list" reply_is "OK" ALERTS
check "ALERT-REPLY to an unknown alert" reply_is "ERROR unknown alert" ALERT-REPLY 1 force
check "CONNECT a manual profile" reply_is "OK" CONNECT manual
eventually "it launches tundra-dhcpc" 3 launched 2
check "STATUS shows it connecting" status_has "iface $IF2 state connecting profile manual address - trusted yes override - alert - ssid - bssid -"

check "DISCONNECT a missing interface" reply_is "ERROR no such interface" DISCONNECT tnd9
pid=$(child_pid casa)
check "DISCONNECT" reply_is "OK" DISCONNECT "$IF"
eventually "dhcpc gets SIGUSR1" 3 grep -q "^$pid USR1\$" "$DHCPC_FAKE_DIR/signals"
eventually "and the subscriber gets event disconnected" 3 has_event "event disconnected $IF casa"
eventually "STATUS shows it idle" 3 status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"

check "a line too long closes the connection" sh -c "[ \"\$(printf '%01100d\n' 0 | $TOOLS/ctl '$SOCK')\" = 'ERROR line too long' ]"
check "several commands in one write" sh -c "[ \"\$(printf 'ALERTS\r\nALERTS\n' | $TOOLS/ctl '$SOCK' | tr '\n' ' ')\" = 'OK OK ' ]"
# A client of an earlier check may still hold its slot when these arrive, and
# then the server rightly turns one of them away: top up until 15 stay.
pids=
tries=0
while [ "$tries" -lt 20 ]; do
	live=
	n=0
	for p in $pids; do
		if kill -0 "$p" 2>/dev/null; then
			live="$live $p"
			n=$((n + 1))
		fi
	done
	pids=$live
	[ "$n" -eq 15 ] && break
	while [ "$n" -lt 15 ]; do
		"$TOOLS/ctl" -f "$SOCK" SUBSCRIBE >/dev/null 2>&1 &
		pids="$pids $!"
		n=$((n + 1))
	done
	sleep 0.2
	tries=$((tries + 1))
done
_failed=$FAIL
eventually "a seventeenth client is turned away" 3 reply_is "ERROR too many clients" ALERTS
[ "$FAIL" -eq "$_failed" ] || {
	_alive=0
	for p in $pids $EV_PID; do
		kill -0 "$p" 2>/dev/null && _alive=$((_alive + 1))
	done
	echo "#   clients still connected: $_alive of 16"
	echo "#   the seventeenth got: $(cmd ALERTS | tr '\n' '|')"
}
for p in $pids; do
	kill "$p" 2>/dev/null
	wait "$p" 2>/dev/null
done
eventually "the slots come back" 3 reply_is "OK" ALERTS

ip netns exec "$NS" "$NETWORKD" >"$WORK/second.log" 2>&1
second=$?
check "a second daemon refuses to start" [ "$second" -eq 1 ]
check "and says why" grep -q "another tundra-networkd answers on $SOCK" "$WORK/second.log"
check "the first daemon still answers" reply_is "OK" ALERTS
stop_daemon
expect_exit 0 "the daemon stops"
check "the socket is removed on exit" test ! -e "$SOCK"

setup
cable_profile casa
fake stubborn
sensor run
start_daemon
eventually "a dhcpc that ignores signals is launched" 5 launched 1
wait_log "control socket on" 3
check "DISCONNECT is accepted" reply_is "OK" DISCONNECT "$IF"
expect_log "$IF: tundra-dhcpc did not stop in 5 s, killed" 8 "it is killed after 5 s"
eventually "and the interface ends idle" 3 status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"
check "CONNECT again" reply_is "OK" CONNECT casa
eventually "it is relaunched" 3 launched 2
: >"$LOG"
kill -TERM "$DAEMON_PID"
if wait_daemon 9; then
	ok "the shutdown is not blocked by a stubborn dhcpc"
else
	not_ok "the shutdown is not blocked by a stubborn dhcpc"
fi
expect_exit 0 "and the daemon exits 0"
expect_log "did not stop in 5 s, killed" 1 "after killing it"
summary
