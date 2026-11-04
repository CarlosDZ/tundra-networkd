. "$(dirname "$0")/lib.sh"

ns() {
	printf 'nameserver %s\n' "$@"
}

setup
add_if "$IF2"
write_conf interfaces <<END
$IF {
	metric = 600
}
$IF2 {
	metric = 100
}
END
cable_profile a "$IF"
cable_profile b "$IF2" "10.0.0.3, 10.0.0.4"
fake run
start_daemon
eventually "both profiles are connected" 5 launched 2
eventually "no servers before any lease" 3 resolv_is ""
check "resolv.conf is readable by everyone" sh -c "[ \"\$(stat -c %a '$RUN/resolv.conf')\" = 644 ]"
write_lease "$IF-a" "10.0.0.1,10.0.0.2"
eventually "a lease brings its servers" 3 resolv_is "$(ns 10.0.0.1 10.0.0.2)"
write_lease "$IF2-b" "10.0.0.9"
eventually "the lowest metric goes first, three servers at most" 3 resolv_is "$(ns 10.0.0.3 10.0.0.4 10.0.0.1)"
expect_log "networkd: dns: 10.0.0.3 10.0.0.4 10.0.0.1" 1 "the change is logged"
rm -f "$LEASES/$IF2-b.lease"
eventually "a lost lease drops its connection" 3 resolv_is "$(ns 10.0.0.1 10.0.0.2)"
write_lease "$IF2-b" "10.0.0.9"
eventually "the lease comes back" 3 resolv_is "$(ns 10.0.0.3 10.0.0.4 10.0.0.1)"
in_ns ip link del "$IF2"
eventually "a connection that ends leaves resolv.conf" 5 resolv_is "$(ns 10.0.0.1 10.0.0.2)"
stop_daemon
check "an orderly stop keeps resolv.conf" resolv_is "$(ns 10.0.0.1 10.0.0.2)"

setup
add_if "$IF2"
cable_profile a "$IF" none
cable_profile b "$IF2"
write_lease "$IF-a" "10.0.0.1"
write_lease "$IF2-b" "10.0.0.5"
fake run
start_daemon
eventually "leases kept from before count at once, dns = none adds nothing" 5 resolv_is "$(ns 10.0.0.5)"
stop_daemon
summary
