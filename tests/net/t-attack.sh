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
eventually "the interface stays stopped" 3 status_has "iface $IF state blocked reason alert profile casa address - trusted no override 1 alert 1"
check "tundra-dhcpc is gone" sh -c "! kill -0 $pid 2>/dev/null"

watching() {
	[ "$(grep -c "gateway is at" "$LOG")" -ge "$1" ]
}

check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "the connection comes back" 15 has_addr
check "with the same address" [ "$(ns_addr)" = "$addr" ]
eventually "and the sensor watches again" 10 watching 2
in_evil "$ARPSEND" "$EVIF" 02:bb:bb:bb:bb:bb "$NET.1" "$addr"
expect_log "blueice-arp\[$IF\]: alert 111/8 warn by tundra-blueice-arp on $IF .* case D " 5 "a second D only warns"
sleep 0.5
check "and the address stays" has_addr
check "STATUS lists the warning" status_has "iface $IF state connected profile casa address $addr/24 trusted no override 1 alert 2"
in_evil "$ARPSEND" "$EVIF" 01:00:5e:00:00:01 "$NET.1" "$addr" 02:cc:cc:cc:cc:cc
expect_log "blueice-arp\[$IF\]: alert 111/8 stop by tundra-blueice-arp on $IF .* case mac-gw " 5 "another case stops again"
eventually "and releases the address" 5 no_addr
stop_daemon
expect_exit 0 "the daemon stops"
summary
