. "$(dirname "$0")/lib.sh"

for bin in "$DHCPC_REAL" "$BIARP_REAL" "$ARPSEND"; do
	if [ ! -x "$bin" ]; then
		not_ok "test build at $bin"
		summary
		exit
	fi
done

setup
cable_profile casa
start_daemon
expect_log "dhcpc\[$IF/casa\]: bound to $NET\." 20 "casa gets a lease"
expect_log "blueice-arp\[$IF\]: gateway is at " 10 "the sensor learns the gateway"
addr=$(ns_addr)
pid=$(child_pid casa)
in_evil "$ARPSEND" "$EVIF" 02:bb:bb:bb:bb:bb "$NET.1" "$addr"
expect_log "blueice-arp\[$IF\]: alert 111/8 stop by tundra-blueice-arp on $IF .* case D " 5 "a second MAC for the gateway raises D"
eventually "dhcpc releases the address" 5 no_addr
check "and keeps the lease" test -e "$LEASES/$IF-casa.lease"
eventually "the interface stays stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no"
check "tundra-dhcpc is gone" sh -c "! kill -0 $pid 2>/dev/null"
stop_daemon
expect_exit 0 "the daemon stops"
summary
