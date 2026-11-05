. "$(dirname "$0")/lib.sh"

setup
add_if "$IF2"
cable_profile casa
cable_profile otra "$IF2"
fake run
sensor run
start_daemon
eventually "both profiles connect" 5 launched 2
wait_log "control socket on" 3
check "no sensor before a lease" [ "$(sensor_launches)" -eq 0 ]
write_lease "$IF-casa" "10.0.0.1" "$NET.200" "$NET.1"
eventually "a lease launches the sensor on its address and gateway" 3 sh -c "grep -q ' watch $IF addr $NET.200 gw $NET.1 defend once\$' '$BIARP_FAKE_DIR/launches'"
expect_log "blueice-arp\[$IF\]: fake sensor watching $NET.200" 3 "its output goes to the log with its prefix"
spid=$(last_sensor | cut -d' ' -f1)
write_lease "$IF-casa" "10.0.0.1" "$NET.201" "$NET.1"
eventually "a new address stops the sensor" 3 sensor_got "$spid" TERM
eventually "and launches it on the new one" 3 sh -c "tail -n 1 '$BIARP_FAKE_DIR/launches' | grep -q ' addr $NET.201 '"
spid=$(last_sensor | cut -d' ' -f1)
rm -f "$LEASES/$IF-casa.lease"
eventually "losing the lease stops the sensor" 3 sensor_got "$spid" TERM
sleep 0.5
check "and does not bring it back" [ "$(sensor_launches)" -eq 2 ]

pid=$(child_pid casa)
sensor conflict
write_lease "$IF-casa" "10.0.0.1" "$NET.202" "$NET.1"
eventually "a conflict asks dhcpc for another address" 3 dhcpc_got "$pid" USR2
expect_log "$IF: address conflict, asking tundra-dhcpc for another" 1 "and says so"
sleep 0.5
check "the sensor does not come back on the conflicting address" [ "$(sensor_launches)" -eq 3 ]
write_lease "$IF-casa" "10.0.0.1" "$NET.203" "$NET.1"
eventually "but does on the next one" 3 sensor_launched 4

sensor exit1
spid=$(last_sensor | cut -d' ' -f1)
write_lease "$IF-casa" "10.0.0.1" "$NET.204" "$NET.1"
expect_log "$IF: relaunching tundra-blueice-arp in 1000 ms" 3 "a failing sensor is relaunched with backoff"
expect_log "$IF: tundra-blueice-arp failed 3 times in a row" 6 "the third failure in a row"
eventually "stops the connection" 3 dhcpc_got "$pid" USR1
eventually "and leaves it stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override - alert 1 ssid - bssid -"
check "the other interface is not touched" status_has "iface $IF2 state connecting profile otra address - trusted no override - alert - ssid - bssid -"
stop_daemon
expect_exit 0 "the daemon stops"

setup
add_if "$IF2"
cable_profile casa
cable_profile otra "$IF2"
fake run
sensor stop:D:8
start_daemon
eventually "both profiles connect" 5 launched 2
wait_log "control socket on" 3
pid=$(child_pid casa)
pid2=$(child_pid otra)
write_lease "$IF-casa" "10.0.0.1" "$NET.200" "$NET.1"
expect_log "blueice-arp\[$IF\]: alert 111/8 stop by tundra-blueice-arp on $IF " 3 "the sensor raises a stop of level 8"
eventually "dhcpc releases that interface" 3 dhcpc_got "$pid" USR1
eventually "which stays stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override 1 alert 1 ssid - bssid -"
sleep 0.5
check "the other interface is not touched" sh -c "! grep -q '^$pid2 ' '$DHCPC_FAKE_DIR/signals'"
check "nothing autoconnects on the stopped interface" [ "$(launches)" -eq 2 ]
check "and the stopped profile cannot be connected" reply_is "ERROR alert pending, answer it first" CONNECT casa
stop_daemon

setup
add_if "$IF2"
cable_profile casa
cable_profile otra "$IF2"
fake run
sensor stop:D:9
start_daemon
eventually "both profiles connect" 5 launched 2
wait_log "control socket on" 3
pid=$(child_pid casa)
pid2=$(child_pid otra)
write_lease "$IF-casa" "10.0.0.1" "$NET.200" "$NET.1"
eventually "level 9 releases this interface" 3 dhcpc_got "$pid" USR1
eventually "and the other one" 3 dhcpc_got "$pid2" USR1
eventually "both stay stopped" 3 status_has "iface $IF2 state blocked reason alert profile otra address - trusted no override 1 alert - ssid - bssid -"
stop_daemon

setup
cable_profile casa
fake run
sensor forge
start_daemon
eventually "casa connects" 5 launched 1
wait_log "control socket on" 3
pid=$(child_pid casa)
write_lease "$IF-casa" "10.0.0.1" "$NET.200" "$NET.1"
expect_log "$IF: alert by tundra-dhcpc discarded on the pipe of tundra-blueice-arp" 3 "an alert signed by another package is discarded"
sleep 0.5
check "and the connection goes on" sh -c "! grep -q '^$pid ' '$DHCPC_FAKE_DIR/signals'"
stop_daemon

setup
cable_profile casa
fake forge
start_daemon
expect_log "$IF: alert by tundra-evil discarded on the pipe of tundra-dhcpc" 5 "dhcpc cannot pass an alert from an unknown package"
check "and keeps running" status_has "iface $IF state connecting profile casa address - trusted no override - alert - ssid - bssid -"
stop_daemon

setup
cable_profile casa
fake alert
start_daemon
expect_log "dhcpc\[$IF/casa\]: alert 111/4 stop by tundra-blueice-arp " 5 "the probe of dhcpc raises a stop"
eventually "the interface stays stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override - alert 1 ssid - bssid -"
sleep 1.5
check "without relaunching dhcpc" [ "$(launches)" -eq 1 ]

fake run
sensor run
stop_daemon
setup
cable_profile casa
fake run
sensor run
start_daemon
eventually "casa connects" 5 launched 1
write_lease "$IF-casa" "10.0.0.1" "$NET.200" "$NET.1"
eventually "the sensor runs" 3 sensor_launched 1
spid=$(last_sensor | cut -d' ' -f1)
stop_daemon
expect_exit 0 "the daemon stops"
check "after stopping the sensor" sensor_got "$spid" TERM
summary
