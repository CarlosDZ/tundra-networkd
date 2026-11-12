. "$(dirname "$0")/lib.sh"

WIRED="address dhcp dns lease autoconnect yes trusted no"

profile_is() {
	[ "$(cmd PROFILE "$1" | tr '\n' '|')" = "$2|OK|" ]
}

file_mode() {
	[ "$(stat -c %a "$CONF/$1.tnc")" = "$2" ]
}

file_has() {
	grep -qxF -- "$2" "$CONF/$1.tnc"
}

same_file() {
	cmp -s "$CONF/$1.tnc" "$WORK/$1.before"
}

no_leftovers() {
	[ -z "$(find "$CONF" "$(dirname "$CONF")" -maxdepth 1 -name '*.tmp')" ]
}

listed() {
	cmd PROFILES | grep -q "^profile $1 "
}

unlisted() {
	! listed "$1"
}

wifi_addr() {
	in_ns ip -4 -o addr show dev "$WIF" | grep -q "inet $WNET\."
}

# 128 bytes, every one outside the safe set: the longest value of a request.
long_pass=$(printf '%064d' 0 | sed 's/0/%C3%A9/g')
long_raw=$(printf '%064d' 0 | sed 's/0/é/g')

setup
write_conf 000-template.tnc <<END
# template
END
fake run
sensor run
start_daemon
expect_log "control socket on $SOCK" 5 "the control socket is up"
subscribe
eventually "subscribed" 3 has_event OK

check "PROFILE of an unknown profile" reply_is "ERROR unknown profile" PROFILE casa
check "PROFILE-SET creates a wired profile" reply_is "OK" PROFILE-SET casa interface "$IF" $WIRED
check "the file is 0600" file_mode casa 600
check "with the text of a profile" file_has casa "	interface = $IF"
eventually "the reload connects it" 5 launched 1
check "PROFILE shows every key" profile_is casa "profile casa interface $IF ssid - security - passphrase - address dhcp gateway - dns lease autoconnect yes trusted no mac -"
check "PROFILES lists it" listed casa
expect_log "networkd: profile casa written by request" 1 "the log names the profile"

pid=$(child_pid casa)
check "PROFILE-SET replaces the whole profile" reply_is "OK" PROFILE-SET casa interface "$IF" address dhcp dns 9.9.9.9,1.1.1.1 autoconnect yes trusted yes
eventually "the connection is released" 3 dhcpc_got "$pid" USR1
eventually "and started again" 3 launched 2
check "PROFILE shows the new values" profile_is casa "profile casa interface $IF ssid - security - passphrase - address dhcp gateway - dns 9.9.9.9,1.1.1.1 autoconnect yes trusted yes mac -"
write_lease "$IF-casa" "10.0.0.1"
eventually "a lease connects it" 3 has_event "event connected $IF casa address $NET.200/24"

cp "$CONF/casa.tnc" "$WORK/casa.before"
check "an invalid value answers with the reason" reply_is "ERROR autoconnect must be yes or no" PROFILE-SET casa interface "$IF" address dhcp dns lease autoconnect maybe trusted no
check "and leaves the profile as it was" same_file casa
check "a missing key too" reply_is "ERROR missing trusted" PROFILE-SET casa interface "$IF" address dhcp dns lease autoconnect yes
check "an invalid new profile" reply_is "ERROR address needs a prefix: 10.0.0.1" PROFILE-SET nuevo interface "$IF" address 10.0.0.1 dns none autoconnect no trusted no
check "is not written" test ! -e "$CONF/nuevo.tnc"
check "a value cannot carry another line" reply_is "ERROR invalid value for address" PROFILE-SET nuevo interface "$IF" address dhcp%0A%09trusted%20=%20yes dns none autoconnect no trusted no
check "a badly encoded value" reply_is "ERROR invalid value for dns" PROFILE-SET nuevo interface "$IF" address dhcp dns no%ne autoconnect no trusted no
check "nothing was written" test ! -e "$CONF/nuevo.tnc"

usage="ERROR usage: PROFILE-SET <profile> <key> <value> [<key> <value>]..."
check "an unknown key" reply_is "$usage" PROFILE-SET nuevo interface "$IF" color red
check "a repeated key" reply_is "$usage" PROFILE-SET nuevo interface "$IF" interface "$IF2"
check "a key without value" reply_is "$usage" PROFILE-SET nuevo interface "$IF" address
check "PROFILE-SET without pairs" reply_is "$usage" PROFILE-SET nuevo
check "PROFILE without a name" reply_is "ERROR usage: PROFILE <profile>" PROFILE
check "PROFILE-DELETE without a name" reply_is "ERROR usage: PROFILE-DELETE <profile>" PROFILE-DELETE

check "a name that leaves the directory" reply_is "ERROR invalid profile name" PROFILE-SET ../fuera interface "$IF" $WIRED
check "writes nothing outside" test ! -e "$(dirname "$CONF")/fuera.tnc"
check "a name with a slash" reply_is "ERROR invalid profile name" PROFILE-SET a/b interface "$IF" $WIRED
check "a hidden name" reply_is "ERROR invalid profile name" PROFILE-SET .oculto interface "$IF" $WIRED
check "an encoded name is not decoded" reply_is "ERROR invalid profile name" PROFILE-SET %2E%2E%2Ffuera interface "$IF" $WIRED
check "the template cannot be replaced" reply_is "ERROR reserved profile name" PROFILE-SET 000-template interface "$IF" $WIRED
check "nor deleted" reply_is "ERROR reserved profile name" PROFILE-DELETE 000-template
check "and is still there" grep -qx "# template" "$CONF/000-template.tnc"
: >"$(dirname "$CONF")/fuera.tnc"
check "PROFILE-DELETE cannot leave the directory" reply_is "ERROR invalid profile name" PROFILE-DELETE ../fuera
check "the file outside is still there" test -e "$(dirname "$CONF")/fuera.tnc"
rm -f "$(dirname "$CONF")/fuera.tnc"
check "PROFILE-DELETE of an unknown profile" reply_is "ERROR unknown profile" PROFILE-DELETE nuevo
check "no temporary file is left" no_leftovers

check "a WiFi profile with the longest passphrase" reply_is "OK" PROFILE-SET red interface tndw9 ssid caf%C3%A9%20%22x%22 security wpa3 passphrase "$long_pass" $WIRED
check "its file is 0600" file_mode red 600
check "with the ssid escaped" file_has red '	ssid = "café \"x\""'
check "and the passphrase" file_has red "	passphrase = \"$long_raw\""
check "PROFILE says only that it is set" profile_is red "profile red interface tndw9 ssid caf%C3%A9%20%22x%22 security wpa3 passphrase set address dhcp gateway - dns lease autoconnect yes trusted no mac -"
check "PROFILE-SET without passphrase keeps it" reply_is "OK" PROFILE-SET red interface tndw9 ssid caf%C3%A9%20%22x%22 security wpa3 address dhcp dns lease autoconnect no trusted no
check "the new values are written" file_has red '	autoconnect = no'
check "with the passphrase it had" file_has red "	passphrase = \"$long_raw\""
cp "$CONF/red.tnc" "$WORK/red.before"
check "but it is not carried to another network" reply_is "ERROR missing passphrase" PROFILE-SET red interface tndw9 ssid otra security wpa3 address dhcp dns lease autoconnect no trusted no
check "which leaves the profile as it was" same_file red
check "a new passphrase replaces it" reply_is "OK" PROFILE-SET red interface tndw9 ssid otra security wpa2 passphrase top%20secret%20%23%5C address dhcp dns lease autoconnect no trusted no
check "escaped in the file" file_has red '	passphrase = "top secret #\\"'
cp "$CONF/red.tnc" "$WORK/red.before"
check "a passphrase wpa2 does not take" reply_is "ERROR wpa2 passphrase must have 8 to 63 characters" PROFILE-SET red interface tndw9 ssid otra security wpa2 passphrase corta address dhcp dns lease autoconnect no trusted no
check "leaves the profile as it was" same_file red
check "a passphrase named without value is missing" reply_is "ERROR missing passphrase" PROFILE-SET red interface tndw9 ssid otra security wpa2 passphrase - address dhcp dns lease autoconnect no trusted no
check "a new WiFi profile needs one" reply_is "ERROR missing passphrase" PROFILE-SET red2 interface tndw9 ssid otra security wpa3 address dhcp dns lease autoconnect no trusted no
check "a network that turns open" reply_is "OK" PROFILE-SET red interface tndw9 ssid otra security open address dhcp dns lease autoconnect no trusted no
check "drops its passphrase" sh -c "! grep -q passphrase '$CONF/red.tnc'"
check "and PROFILE shows it" profile_is red "profile red interface tndw9 ssid otra security open passphrase - address dhcp gateway - dns lease autoconnect no trusted no mac -"
check "no passphrase reached the log" sh -c "! grep -q -e 'top secret' -e corta -e éééé '$LOG'"
check "nor the subscribers" sh -c "! grep -q -e 'top secret' -e corta -e passphrase '$WORK/events'"

write_conf mano.tnc <<END
match {
	interface = $IF
}
END
check "RELOAD with a broken profile written by hand" reply_is "OK" RELOAD
check "PROFILE shows its error" sh -c "$TOOLS/ctl '$SOCK' 'PROFILE mano' | grep -q '^profile mano invalid -- '"
check "PROFILE-SET repairs it" reply_is "OK" PROFILE-SET mano interface "$IF2" address dhcp dns none autoconnect no trusted no
check "PROFILE shows it valid" profile_is mano "profile mano interface $IF2 ssid - security - passphrase - address dhcp gateway - dns none autoconnect no trusted no mac -"

pid=$(child_pid casa)
check "PROFILE-DELETE" reply_is "OK" PROFILE-DELETE casa
check "removes the file" test ! -e "$CONF/casa.tnc"
eventually "the reload disconnects it" 3 dhcpc_got "$pid" USR1
eventually "with event disconnected" 3 has_event "event disconnected $IF casa"
check "PROFILES no longer lists it" unlisted casa
check "PROFILE neither" reply_is "ERROR unknown profile" PROFILE casa
check "deleting it twice" reply_is "ERROR unknown profile" PROFILE-DELETE casa
check "the others stay" listed mano
expect_log "networkd: profile casa deleted by request" 1 "the log names the deleted profile"

check "the longest request fits a line" reply_is "OK" PROFILE-SET "$(printf '%064d' 0)" interface tndw9abcdefghi ssid "$(printf '%032d' 0 | sed 's/0/%FF/g')" security wpa3 passphrase "$long_pass" address 192.168.100.150/24 gateway 192.168.100.100 dns 192.168.100.101,192.168.100.102,192.168.100.103 autoconnect yes trusted yes mac 02:12:34:56:78:9a
check "a line too long closes the connection" sh -c "[ \"\$(printf 'PROFILE-SET x passphrase %01100d\n' 0 | $TOOLS/ctl '$SOCK')\" = 'ERROR line too long' ]"
check "no temporary file at the end" no_leftovers
stop_daemon
expect_exit 0 "the daemon stops"

# As the unit runs it: not root, writing only where the directory lets it.
setup
uid=$(stat -c %u "$ROOT")
if [ "$uid" -eq 0 ]; then
	echo "# skip: the checkout belongs to root, no other user to run as"
else
	gid=$(stat -c %g "$ROOT")
	mkdir -p "$(dirname "$SOCK")" "$LEASES"
	owner=$(stat -c %u:%g "$LEASES")
	chown -R "$uid" "$(dirname "$SOCK")" "$RUN"
	chown "$uid" "$LEASES"
	chown "root:$gid" "$CONF"
	chmod 2770 "$CONF"
	wifi_profile mano mano wpa3 "written by hand"
	chown "root:$gid" "$CONF/mano.tnc"
	chmod 640 "$CONF/mano.tnc"
	: >"$LOG"
	caps=+net_admin,+net_raw,+net_bind_service
	ip netns exec "$NS" setpriv --reuid "$uid" --regid "$gid" --clear-groups \
		--inh-caps "$caps" --ambient-caps "$caps" "$NETWORKD" >>"$LOG" 2>&1 &
	DAEMON_PID=$!
	expect_log "control socket on $SOCK" 5 "the daemon runs without being root"
	check "a passphrase at 0640 root and its group is accepted" profile_is mano "profile mano interface $WIF ssid mano security wpa3 passphrase set address dhcp gateway - dns lease autoconnect yes trusted no mac -"
	check "PROFILE-SET in a 2770 directory of its group" reply_is "OK" PROFILE-SET red interface "$WIF" ssid red security wpa3 passphrase top%20secret address dhcp dns lease autoconnect no trusted no
	check "the file is 0600 of the daemon" [ "$(stat -c '%a %u' "$CONF/red.tnc")" = "600 $uid" ]
	check "and loads with its passphrase" profile_is red "profile red interface $WIF ssid red security wpa3 passphrase set address dhcp gateway - dns lease autoconnect no trusted no mac -"
	check "it replaces a profile written by root" reply_is "OK" PROFILE-SET mano interface "$WIF" ssid mano security wpa3 address dhcp dns lease autoconnect no trusted no
	check "keeping its passphrase" file_has mano '	passphrase = "written by hand"'
	check "PROFILE-DELETE as the daemon" reply_is "OK" PROFILE-DELETE mano
	chmod 640 "$CONF/red.tnc"
	chgrp 65534 "$CONF/red.tnc"
	check "RELOAD" reply_is "OK" RELOAD
	check "a passphrase another group can read is rejected" sh -c "$TOOLS/ctl '$SOCK' 'PROFILE red' | grep -q '^profile red invalid -- credentials in a file readable by another group or by others'"
	chmod 755 "$CONF"
	check "a directory it cannot write" reply_is "ERROR cannot write the profile" PROFILE-SET otro interface "$IF" $WIRED
	check "leaves no file" test ! -e "$CONF/otro.tnc"
	check "nor a temporary one" no_leftovers
	check "PROFILE-DELETE fails there too" reply_is "ERROR cannot delete the profile" PROFILE-DELETE red
	check "and the file stays" test -e "$CONF/red.tnc"
	check "the daemon still answers" reply_is "OK" ALERTS
	stop_daemon
	expect_exit 0 "and stops cleanly"
	chown "$owner" "$LEASES"
fi

if ! wifi_ready || [ ! -x "$DHCPC_REAL" ]; then
	echo "# skip: WiFi prerequisites or the tundra-dhcpc test build are missing"
	summary
	exit
fi

setup
if ! wifi_setup; then
	not_ok "three mac80211_hwsim radios in their namespaces"
	summary
	exit
fi
physical "$WIF"
mkdir -p "$SYS/$WIF/phy80211"
check "an access point starts" start_ap "$WAP1" tnd-casa wpa3 "clave de casa"
start_daemon
expect_log "$WIF: wpa_supplicant started" 5 "wpa_supplicant runs without any profile"
check "PROFILE-SET creates the WiFi profile" reply_is "OK" PROFILE-SET casa interface "$WIF" ssid tnd-casa security wpa3 passphrase clave%20de%20casa address dhcp dns lease autoconnect yes trusted no
eventually "the reload joins the network and gets an address" 40 wifi_addr
eventually "STATUS shows the connection" 5 sh -c "$TOOLS/ctl '$SOCK' STATUS | grep -q '^iface $WIF state connected profile casa '"
check "the passphrase is not in the log" sh -c "! grep -q 'clave de casa' '$LOG'"
check "PROFILE-DELETE of the connected profile" reply_is "OK" PROFILE-DELETE casa
eventually "the reload leaves the network" 15 sh -c "! $TOOLS/ctl '$SOCK' STATUS | grep -q '^iface $WIF state [a-z]* profile casa '"
eventually "and releases its address" 15 sh -c "! ip netns exec '$NS' ip -4 -o addr show dev '$WIF' | grep -q 'inet $WNET\.'"
stop_daemon
summary
