. "$(dirname "$0")/lib.sh"

wcli() {
	wpa_cli -p "$RUN/wpa" -i "$WIF" "$@"
}

nets_are() {
	[ "$(wcli list_networks | tail -n +2 |
		awk -F '\t' '{ print $2 ($4 ? " " $4 : "") }')" = "$1" ]
}

net_id() {
	for _i in $(wcli list_networks | tail -n +2 | cut -f 1); do
		if [ "$(wcli get_network "$_i" id_str)" = "\"$1\"" ]; then
			echo "$_i"
			return
		fi
	done
}

net_is() {
	_id=$(net_id "$1")
	[ -n "$_id" ] && [ "$(wcli get_network "$_id" "$2")" = "$3" ]
}

on_profile() {
	wcli status | grep -qx "wpa_state=COMPLETED" &&
		wcli status | grep -qx "id_str=$1"
}

wpa_state_is() {
	wcli status | grep -qx "wpa_state=$1"
}

sees() {
	wcli scan_results | grep -q "	$1\$"
}

associations() {
	grep -c "$WIF: associated with" "$LOG"
}

if ! wifi_ready; then
	not_ok "WiFi prerequisites: hostapd, iw, wpa_cli, $WPA_REAL and mac80211_hwsim"
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
wifi_profile 10-casa tnd-wpa3 wpa3 "correct horse wpa3"
wifi_profile 20-cafe "tnd open" open "" no
wifi_profile 30-twin tnd-twin wpa3 "correct horse twin"
wifi_profile 40-hotel tnd-wpa2 wpa2 "correct horse wpa2" no
wifi_profile 50-cable tnd-wpa3 wpa3 "correct horse wpa3" yes "$IF"

start_daemon
expect_log "$WIF: wpa_supplicant ready" 10 "wpa_supplicant is ready"
eventually "the WiFi profiles of the interface become its networks, autoconnect = no disabled" 5 nets_are "tnd-wpa3
tnd open [DISABLED]
tnd-twin
tnd-wpa2 [DISABLED]"
check "the first profile has the highest priority" net_is 10-casa priority 4
check "and the last one the lowest" net_is 40-hotel priority 1
check "wpa3 is SAE" net_is 10-casa key_mgmt SAE
check "with PMF required" net_is 10-casa ieee80211w 2
check "wpa2 is WPA-PSK" net_is 40-hotel key_mgmt WPA-PSK
check "with PMF if the access point offers it" net_is 40-hotel ieee80211w 1
check "open has no key" net_is 20-cafe key_mgmt NONE

check "a WPA2 twin of the WPA3 profile" start_ap "$WAP2" tnd-twin wpa2 "correct horse twin"
eventually "the station sees it" 20 sees tnd-twin
sleep 5
check "the wpa3 profile does not even try the twin" sh -c "! wpa_cli -p '$RUN/wpa' -i '$WIF' status | grep -q '^wpa_state=\(COMPLETED\|ASSOCIAT\|AUTHENTICAT\|4WAY\)'"
check "nor joins it" test "$(associations)" -eq 0

check "a WPA3 access point" start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3"
eventually "the autoconnect profile joins it" 20 on_profile 10-casa
check "with SAE" sh -c "wpa_cli -p '$RUN/wpa' -i '$WIF' status | grep -qx key_mgmt=SAE"

stop_ap "$WAP2"
check "an open access point" start_ap "$WAP2" "tnd open" open
check "CONNECT to a disabled profile" reply_is "OK" CONNECT 20-cafe
eventually "selects that network" 20 on_profile 20-cafe
check "and leaves the others disabled" net_is 10-casa disabled 1

check "DISCONNECT" reply_is "OK" DISCONNECT "$WIF"
eventually "disconnects wpa_supplicant" 5 wpa_state_is DISCONNECTED
sleep 2
check "and it stays disconnected" wpa_state_is DISCONNECTED
check "CONNECT again" reply_is "OK" CONNECT 10-casa
eventually "reconnects with that profile" 20 on_profile 10-casa

networks_have() {
	cmd NETWORKS "$WIF" | grep -qx "$1"
}

check "NETWORKS on a cable" reply_is "ERROR no such wifi interface" NETWORKS "$IF"
check "SCAN on a cable" reply_is "ERROR no such wifi interface" SCAN "$IF"
subscribe
eventually "subscribed" 3 has_event OK
check "SCAN" reply_is "OK" SCAN "$WIF"
eventually "its results are announced" 20 has_event "event scan $WIF"
unsubscribe
check "the association survives the scan" on_profile 10-casa
check "NETWORKS starts with the age of the scan" sh -c "$TOOLS/ctl '$SOCK' NETWORKS | head -n 1 | grep -qx 'scan $WIF age [0-9]*'"
check "lists the access point with its profile" networks_have "network $WIF ssid tnd-wpa3 bssid [0-9a-f:]* freq [0-9]* signal -[0-9]* security wpa3 profile 10-casa"
_failed=$FAIL
check "and the open one, with its ssid encoded" networks_have "network $WIF ssid tnd%20open bssid [0-9a-f:]* freq [0-9]* signal -[0-9]* security open profile 20-cafe"
[ "$FAIL" -eq "$_failed" ] || {
	"$TOOLS/ctl" "$SOCK" NETWORKS | sed 's/^/#   /'
	wcli scan_results | sed 's/^/#   wpa: /'
}
check "and ends with OK" sh -c "$TOOLS/ctl '$SOCK' NETWORKS | tail -n 1 | grep -qx OK"

before=$(associations)
casa=$(net_id 10-casa)
rm -f "$CONF/30-twin.tnc"
wifi_profile 20-cafe "tnd cafe" open "" no
wifi_profile 60-nuevo "tnd nuevo" open "" no
check "RELOAD with a profile removed, one changed and one new" reply_is "OK" RELOAD
eventually "the networks follow the profiles" 5 sh -c "[ -z \"\$(wpa_cli -p '$RUN/wpa' -i '$WIF' list_networks | grep tnd-twin)\" ]"
check "the changed one is replaced" net_is 20-cafe ssid '"tnd cafe"'
check "the new one is added" net_is 60-nuevo priority 1
check "priorities follow the new order" net_is 40-hotel priority 2
check "the untouched network keeps its id" test "$(net_id 10-casa)" = "$casa"
sleep 2
check "and its association" on_profile 10-casa
check "without associating again" test "$(associations)" -eq "$before"

wmac() {
	in_ns cat "/sys/class/net/$WIF/address"
}

check "a network without mac uses the address of the interface" net_is 10-casa mac_addr 0
write_conf 10-casa.tnc <<END
match {
	interface = $WIF
	ssid = "tnd-wpa3"
	security = wpa3
}
apply {
	passphrase = "correct horse wpa3"
	address = dhcp
	dns = lease
	autoconnect = yes
	trusted = no
	mac = 02:12:34:56:78:9a
}
END
check "RELOAD with a mac for that network" reply_is "OK" RELOAD
eventually "the network gets a dedicated address" 5 net_is 10-casa mac_addr 3
eventually "and joins again" 30 on_profile 10-casa
check "with that mac" test "$(wmac)" = 02:12:34:56:78:9a

check "a WiFi profile does not CONNECT on a cable" reply_is "ERROR profile does not match" CONNECT 50-cable "$IF"
stop_daemon
summary
