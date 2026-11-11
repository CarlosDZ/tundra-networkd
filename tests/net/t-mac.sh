. "$(dirname "$0")/lib.sh"

mac_of() {
	in_ns cat "/sys/class/net/$IF/address"
}

mac_profile() {
	write_conf casa.tnc <<END
match {
	interface = $IF
}
apply {
	address = dhcp
	dns = lease
	autoconnect = yes
	trusted = no
$1
}
END
}

# Unicast and locally administered: the second digit is 2, 6, a or e.
random_looking() {
	echo "$1" | grep -q '^[0-9a-f][26ae]:'
}

setup
factory=$(mac_of)
mac_profile "	mac = 02:12:34:56:78:9a"
fake run
sensor run
start_daemon
eventually "the profile connects" 5 launched 1
check "with the mac of the profile" test "$(mac_of)" = 02:12:34:56:78:9a
check "set before tundra-dhcpc starts" sh -c "grep -n 'mac set to\|tundra-dhcpc started' '$LOG' | head -n 1 | grep -q 'mac set to 02:12:34:56:78:9a'"
check "and the interface is up" is_up

mac_profile "	mac = random"
check "RELOAD with mac = random" reply_is "OK" RELOAD
eventually "reconnects" 5 launched 2
first=$(mac_of)
check "with a random mac ($first)" random_looking "$first"
check "that is not the fixed one" test "$first" != 02:12:34:56:78:9a
check "DISCONNECT" reply_is "OK" DISCONNECT "$IF"
eventually "is released" 3 status_has "iface $IF state idle profile - address - trusted - override - alert - ssid - bssid -"
check "CONNECT" reply_is "OK" CONNECT casa
eventually "connects again" 5 launched 3
second=$(mac_of)
check "with another random mac ($second)" sh -c "[ '$second' != '$first' ] && echo '$second' | grep -q '^[0-9a-f][26ae]:'"

write_conf interfaces <<END
$IF {
	mac = 02:aa:bb:cc:dd:ee
}
END
mac_profile ""
check "RELOAD with a base mac and none in the profile" reply_is "OK" RELOAD
eventually "reconnects" 5 launched 4
check "with the base of the interface" test "$(mac_of)" = 02:aa:bb:cc:dd:ee

mac_profile "	mac = keep"
check "RELOAD with mac = keep" reply_is "OK" RELOAD
eventually "reconnects" 5 launched 5
check "an interface without hardware address keeps the one it has" test "$(mac_of)" = 02:aa:bb:cc:dd:ee

: >"$TIP_FAIL.mac"
mac_profile "	mac = 02:12:34:56:78:9b"
check "RELOAD while the mac cannot be set" reply_is "OK" RELOAD
expect_log "$IF: cannot set the mac, not connecting" 5 "the failure is reported"
sleep 1
check "and tundra-dhcpc is not launched" test "$(launches)" -eq 5
check "nor the mac changed" test "$(mac_of)" = 02:aa:bb:cc:dd:ee
rm -f "$TIP_FAIL.mac"
eventually "once it can be set, the connection starts" 15 launched 6
check "with the mac asked for" test "$(mac_of)" = 02:12:34:56:78:9b
stop_daemon
expect_exit 0 "the daemon stops"
[ "$FAIL" -eq 0 ] || {
	echo "#   factory mac $factory, now $(mac_of)"
	grep -v "^networkd: dhcpc\[" "$LOG" | tail -n 40 | sed 's/^/#   /'
}
summary
