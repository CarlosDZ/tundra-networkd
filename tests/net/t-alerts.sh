. "$(dirname "$0")/lib.sh"

lease() {
	write_lease "$1-$2" "10.0.0.1" "$3" "$NET.1"
}

event() {
	grep -q -- "$1" "$WORK/events"
}

casa_launches() {
	grep -c 'lease-key casa$' "$DHCPC_FAKE_DIR/launches"
}

last_sensor_is() {
	tail -n 1 "$BIARP_FAKE_DIR/launches" | grep -q -- "$1\$"
}

setup
add_if "$IF2"
cable_profile casa
cable_profile otra "$IF2"
fake run
sensor run
start_daemon
eventually "both profiles connect" 5 launched 2
wait_log "control socket on" 3
subscribe
eventually "subscribed" 3 has_event OK
pid=$(child_pid casa)
sensor stop:D:8
lease "$IF" casa "$NET.200"
eventually "a stop reaches the subscribers with id and profile" 3 event "^ALERT 1 casa alert 111/8 stop by tundra-blueice-arp on $IF "
check "ALERTS lists it" sh -c "$TOOLS/ctl '$SOCK' ALERTS | head -n 1 | grep -q '^ALERT 1 casa alert 111/8 stop '"
check "it is kept in /run" grep -q "^ALERT 1 casa alert 111/8 stop " "$RUN/alerts"
eventually "STATUS ties it to the interface and lowers trust" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override 1 alert 1 ssid - bssid -"
check "the other interface is not touched" status_has "iface $IF2 state connecting profile otra address - trusted no override - alert - ssid - bssid -"
check "an option the alert does not offer" reply_is "ERROR option not offered by that alert" ALERT-REPLY 1 ack
check "an unknown alert" reply_is "ERROR unknown alert" ALERT-REPLY 99 force
check "an id that is not a number" reply_is "ERROR unknown alert" ALERT-REPLY 1x force

check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "the subscribers learn the alert is closed" 3 has_event "event alert-closed 1 force"
eventually "brings dhcpc back for the same profile" 3 launched 3
check "and closes the alert" reply_is "OK" ALERTS
check "and takes it out of /run" sh -c "! grep -q '^ALERT 1 ' '$RUN/alerts'"
lease "$IF" casa "$NET.200"
eventually "the sensor comes back accepting D/8" 3 last_sensor_is " defend once accept D/8"
eventually "the forced connection stays untrusted" 3 status_has "iface $IF state connected profile casa address $NET.200/24 trusted no override 1 alert - ssid - bssid -"

pid=$(child_pid casa)
sensor stop:D:8
lease "$IF" casa "$NET.201"
eventually "a second D only warns" 3 event "^ALERT 2 casa alert 111/8 warn "
sleep 0.5
check "and the connection goes on" sh -c "! grep -q '^$pid ' '$DHCPC_FAKE_DIR/signals'"
check "STATUS lists the pending warning" status_has "iface $IF state connected profile casa address $NET.201/24 trusted no override 1 alert 2 ssid - bssid -"
check "ack closes a warning" reply_is "OK" ALERT-REPLY 2 ack

sensor stop:C:6
lease "$IF" casa "$NET.202"
eventually "another case stops again" 3 event "^ALERT 3 casa alert 111/6 stop "
eventually "and releases the address" 3 dhcpc_got "$pid" USR1
check "disconnect" reply_is "OK" ALERT-REPLY 3 disconnect
eventually "leaves the interface idle" 3 status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"
check "RELOAD" reply_is "OK" RELOAD
sleep 0.5
check "does not autoconnect it" [ "$(casa_launches)" -eq 2 ]
check "an explicit CONNECT does" reply_is "OK" CONNECT casa
eventually "with a new dhcpc" 3 sh -c "[ \$(grep -c 'lease-key casa\$' '$DHCPC_FAKE_DIR/launches') -eq 3 ]"
eventually "and a sensor without the old accepts" 3 last_sensor_is " addr $NET.202 gw $NET.1 defend once"

sensor stop:D:8
lease "$IF" casa "$NET.203"
eventually "a new stop" 3 event "^ALERT 4 casa alert 111/8 stop "
eventually "stops the interface" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override 4 alert 4 ssid - bssid -"
unsubscribe
kill_daemon
check "the alert survives the daemon" grep -q "^ALERT 4 casa " "$RUN/alerts"
start_daemon
expect_log "networkd: 1 pending alerts restored" 5 "a new daemon restores it"
wait_log "control socket on" 3
eventually "and keeps the interface stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override 4 alert 4 ssid - bssid -"
sleep 0.5
check "without launching casa" [ "$(casa_launches)" -eq 3 ]
check "ids go on from the last one" grep -qx 5 "$RUN/alert-id"
check "force after the restart" reply_is "OK" ALERT-REPLY 4 force
eventually "brings casa back" 3 sh -c "[ \$(grep -c 'lease-key casa\$' '$DHCPC_FAKE_DIR/launches') -eq 4 ]"
stop_daemon

setup
add_if "$IF2"
cable_profile casa
cable_profile otra "$IF2"
fake run
sensor run
start_daemon
eventually "both profiles connect" 5 launched 2
wait_log "control socket on" 3
lease "$IF2" otra "$NET.210"
eventually "otra has its sensor" 3 sensor_launched 1
sensor stop:D:9
lease "$IF" casa "$NET.200"
eventually "level 9 stops both" 3 status_has "iface $IF2 state blocked reason alert profile otra address - trusted no override 1 alert - ssid - bssid -"
check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "both connections come back" 3 launched 4
eventually "the origin accepts D/9" 3 grep -q " watch $IF addr $NET.200 gw $NET.1 defend once accept D/9\$" "$BIARP_FAKE_DIR/launches"
eventually "the other one accepts nothing" 3 sh -c "[ \$(grep -c ' watch $IF2 addr $NET.210 gw $NET.1 defend once\$' '$BIARP_FAKE_DIR/launches') -eq 2 ]"
check "and both stay untrusted" status_has "iface $IF2 state connected profile otra address $NET.210/24 trusted no override 1 alert - ssid - bssid -"
stop_daemon

setup
cable_profile casa
fake run
sensor exit1
start_daemon
eventually "casa connects" 5 launched 1
wait_log "control socket on" 3
subscribe
eventually "subscribed" 3 has_event OK
lease "$IF" casa "$NET.200"
eventually "three failures raise a sensor-down stop signed by tundra-networkd" 8 event "^ALERT 1 casa alert 111/5 stop by tundra-networkd on $IF ts [0-9.]* reply force,disconnect info case sensor-down fails 3 -- tundra-blueice-arp keeps failing\$"
eventually "level 5 does not lower trust" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override - alert 1 ssid - bssid -"
check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "brings casa back" 3 launched 2
pid=$(child_pid casa)
eventually "three more failures only warn" 10 event "^ALERT 2 casa alert 111/5 warn by tundra-networkd "
sleep 0.5
check "and the connection goes on" sh -c "! grep -q '^$pid ' '$DHCPC_FAKE_DIR/signals'"
stop_daemon

setup
cable_profile casa
fake run
sensor warn:mac-net:2
start_daemon
eventually "casa connects" 5 launched 1
wait_log "control socket on" 3
pid=$(child_pid casa)
lease "$IF" casa "$NET.200"
eventually "a warning is pending" 3 status_has "iface $IF state connected profile casa address $NET.200/24 trusted no override - alert 1 ssid - bssid -"
check "disconnect on a warning" reply_is "OK" ALERT-REPLY 1 disconnect
eventually "disconnects that connection" 3 dhcpc_got "$pid" USR1
eventually "and leaves the interface idle" 3 status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"
stop_daemon
summary
