. "$(dirname "$0")/lib.sh"

if [ ! -x "$DHCPC_REAL" ]; then
	not_ok "tundra-dhcpc test build at $DHCPC_REAL"
	summary
	exit
fi

setup
stop_dnsmasq
start_dnsmasq --dhcp-option=6,"$NET.53"
write_conf interfaces <<END
$IF {
	metric = 300
}
END
cable_profile casa
start_daemon
expect_log "dhcpc\[$IF/casa\]: bound to $NET\." 20 "the profile gets a lease through tundra-dhcpc"
eventually "the address is applied" 3 has_addr
eventually "the default route carries the interface metric" 3 sh -c "ip netns exec $NS ip route show default | grep -q 'metric 300'"
eventually "the lease is stored under the profile key" 3 test -e "$LEASES/$IF-casa.lease"
expect_log "$IF: lease for casa updated" 3 "the daemon sees the lease through inotify"
eventually "the lease servers reach resolv.conf" 3 resolv_is "nameserver $NET.53"

pid=$(child_pid casa)
kill_daemon
eventually "dhcpc stops when the daemon is killed" 5 sh -c "! kill -0 $pid"
check "the address survives the daemon" has_addr
check "the lease survives the daemon" test -e "$LEASES/$IF-casa.lease"

start_daemon
expect_log "dhcpc\[$IF/casa\]: bound to $NET\." 20 "a new daemon binds again"
pid=$(child_pid casa)
stop_daemon
expect_exit 0 "the daemon stops cleanly"
check "dhcpc is gone before the daemon exits" sh -c "! kill -0 $pid"
check "the address survives an orderly stop" has_addr
summary
