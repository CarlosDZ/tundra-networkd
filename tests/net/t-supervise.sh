. "$(dirname "$0")/lib.sh"

setup
cable_profile casa
write_conf roto.tnc <<END
match {
	interface = $IF
}
END
fake run
start_daemon
expect_log "dhcpc\[$IF/casa\]: fake running" 5 "autoconnect launches tundra-dhcpc with the log prefix"
check "dhcpc gets the interface and the lease key" grep -q " $IF lease-key casa\$" "$DHCPC_FAKE_DIR/launches"
check "the child starts with no blocked signals" grep -q '^SigBlk:[[:space:]]*0*$' "$DHCPC_FAKE_DIR/sigs"
check "the child starts with no ignored signals" grep -q '^SigIgn:[[:space:]]*0*$' "$DHCPC_FAKE_DIR/sigs"
check "the child inherits only stdin, stdout and stderr" sh -c "[ \"\$(sort -n '$DHCPC_FAKE_DIR/fds' | tr '\n' ' ')\" = '0 1 2 ' ]"
expect_log "profile roto: " 1 "an invalid profile is reported"

in_ns ip link del "$IF"
expect_log "$IF: removed, waiting for it to come back" 5 "dhcpc exits when the interface goes away"
sleep 1.5
check "no relaunch while the interface is missing" [ "$(launches)" -eq 1 ]
add_if
eventually "dhcpc relaunches when RTM_NEWLINK brings the interface back" 5 launched 2

pid=$(child_pid casa)
kill_daemon
eventually "dhcpc gets SIGTERM when the daemon dies" 3 grep -q "^$pid TERM\$" "$DHCPC_FAKE_DIR/signals"

setup
cable_profile casa
fake run
start_daemon
wait_log "fake running" 5
pid=$(child_pid casa)
stop_daemon
expect_exit 0 "SIGTERM stops the daemon cleanly"
check "dhcpc gets SIGTERM in the orderly stop" grep -q "^$pid TERM\$" "$DHCPC_FAKE_DIR/signals"
check "the daemon waits for dhcpc before exiting" sh -c "! kill -0 $pid"

setup
cable_profile casa
fake exit1
start_daemon
expect_log "relaunching tundra-dhcpc in 1000 ms" 5 "exit 1 schedules a relaunch"
eventually "dhcpc is relaunched with backoff" 6 launched 3
expect_log "relaunching tundra-dhcpc in 2000 ms" 1 "the backoff doubles"
stop_daemon
expect_exit 0 "the daemon stops during a backoff"

setup
cable_profile casa
fake exit2
start_daemon
expect_log "profile casa rejected, waiting for a reload" 5 "exit 2 blocks the profile"
sleep 2
check "exit 2 is not relaunched" [ "$(launches)" -eq 1 ]
stop_daemon

setup
cable_profile 999-dhcp '*'
fake run
start_daemon
wait_log "profiles loaded" 3
sleep 1
check "the reserve profile skips a virtual interface" [ "$(launches)" -eq 0 ]
stop_daemon
physical "$IF"
start_daemon
expect_log "dhcpc\[$IF/999-dhcp\]: fake running" 5 "the reserve profile takes a physical interface"
stop_daemon
summary
