. "$(dirname "$0")/lib.sh"

applied() {
	[ "$(ns_addr)" = "$NET.50" ]
}

alerts_have() {
	"$TOOLS/ctl" "$SOCK" ALERTS | grep -q -- "$1"
}

setup
write_conf interfaces <<END
$IF {
	metric = 300
	table = 100
}
END
static_profile oficina "$NET.50/24"
fake run
sensor run
probe free
start_daemon
wait_log "control socket on" 5
eventually "a free static address is applied" 5 applied
check "after probing it" grep -q "^probe $IF $NET.50\$" "$BIARP_FAKE_DIR/probes"
check "with the default route and its metric" in_ns sh -c "ip route show default | grep -q 'via $NET.1 dev $IF .*metric 300'"
check "the subnet route in table 100" in_ns sh -c "ip route show table 100 | grep -q '$NET.0/24 dev $IF .*src $NET.50'"
check "the default route in table 100" in_ns sh -c "ip route show table 100 | grep -q '^default via $NET.1 dev $IF'"
check "and the rule" in_ns sh -c "ip rule show | grep -q 'from $NET.50 lookup 100'"
eventually "the sensor watches it" 3 grep -q " watch $IF addr $NET.50 gw $NET.1 defend once\$" "$BIARP_FAKE_DIR/launches"
eventually "resolv.conf has its dns" 3 resolv_is "nameserver 9.9.9.9"
check "STATUS shows it connected" status_has "iface $IF state connected profile oficina address $NET.50/24 trusted no override - alert -"
check "no dhcpc is launched" [ "$(launches)" -eq 0 ]

check "DISCONNECT" reply_is "OK" DISCONNECT "$IF"
eventually "removes the address" 3 no_addr
check "the default route" in_ns sh -c "! ip route show default | grep -q ."
check "the rule" in_ns sh -c "! ip rule show | grep -q 'lookup 100'"
check "and the routes of table 100" in_ns sh -c "[ -z \"\$(ip route show table 100)\" ]"
eventually "resolv.conf drops its dns" 3 resolv_is ""
check "CONNECT" reply_is "OK" CONNECT oficina
eventually "applies it again" 5 applied
stop_daemon
check "a daemon that stops leaves it in place" applied

setup
static_profile oficina "$NET.50/24"
fake run
sensor run
probe taken
start_daemon
wait_log "control socket on" 5
eventually "a taken address blocks the interface" 5 status_has "iface $IF state blocked reason taken profile oficina address - trusted no override - alert 1"
check "with static-taken and the MAC that holds it" alerts_have "^ALERT 1 oficina alert 111/2 warn by tundra-networkd on $IF ts [0-9.]* reply ack,disconnect info case static-taken ip $NET.50 mac 02:aa:aa:aa:aa:01 src 02:aa:aa:aa:aa:01 -- static address in use\$"
check "the address is not applied" no_addr
sleep 1.5
check "nothing retries" [ "$(probes)" -eq 1 ]
check "ack" reply_is "OK" ALERT-REPLY 1 ack
check "keeps it blocked" status_has "iface $IF state blocked reason taken profile oficina address - trusted no override - alert -"
check "a reload too" reply_is "OK" RELOAD
sleep 0.5
check "without probing" [ "$(probes)" -eq 1 ]
probe free
check "CONNECT" reply_is "OK" CONNECT oficina
eventually "applies it once it is free" 5 applied
eventually "with its sensor" 3 sensor_launched 1

sensor conflict
check "DISCONNECT" reply_is "OK" DISCONNECT "$IF"
check "CONNECT" reply_is "OK" CONNECT oficina
eventually "the sensor giving the address up" 5 status_has "iface $IF state blocked reason taken profile oficina address - trusted no override - alert 2"
check "removes it" no_addr
check "and warns without src" alerts_have "^ALERT 2 oficina alert 111/2 warn by tundra-networkd on $IF .* info case static-taken ip $NET.50 mac 02:aa:aa:aa:aa:01 -- static address in use\$"
stop_daemon

setup
static_profile oficina "$NET.50/24"
fake run
sensor run
probe free
echo 1 >"$TIP_FAIL"
start_daemon
expect_log "$IF: could not apply the static address" 5 "a failure of tundra-ip is reported"
check "and rolled back" no_addr
expect_log "$IF: retrying the static address in 1000 ms" 1 "and retried"
rm -f "$TIP_FAIL"
eventually "until it applies" 5 applied
stop_daemon

setup
static_profile oficina "$NET.50/24"
fake run
sensor stop:D:8
probe free
start_daemon
wait_log "control socket on" 5
eventually "a stop removes the static address" 5 status_has "iface $IF state blocked reason alert profile oficina address - trusted no override 1 alert 1"
check "from the interface" no_addr
check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "probes and applies it again" 5 applied
check "with a second probe" [ "$(probes)" -eq 2 ]
eventually "and a sensor accepting D/8" 3 sh -c "tail -n 1 '$BIARP_FAKE_DIR/launches' | grep -q ' accept D/8\$'"
stop_daemon

setup
static_profile oficina "$NET.50/24"
fake run
sensor run
probe alert
start_daemon
wait_log "control socket on" 5
eventually "a stop from the probe blocks the interface" 5 status_has "iface $IF state blocked reason alert profile oficina address - trusted no override - alert 1"
check "without applying" no_addr
check "force" reply_is "OK" ALERT-REPLY 1 force
eventually "a forced dispute applies without probing" 5 applied
check "no second probe" [ "$(probes)" -eq 1 ]
eventually "and defends always" 3 sh -c "tail -n 1 '$BIARP_FAKE_DIR/launches' | grep -q ' defend always accept A/4\$'"
stop_daemon
summary
