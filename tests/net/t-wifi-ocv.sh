. "$(dirname "$0")/lib.sh"

wcli() {
	wpa_cli -p "$RUN/wpa" -i "$WIF" "$@"
}

net_is() {
	for _i in $(wcli list_networks | tail -n +2 | cut -f 1); do
		if [ "$(wcli get_network "$_i" id_str)" = "\"$1\"" ]; then
			[ "$(wcli get_network "$_i" "$2")" = "$3" ]
			return
		fi
	done
	return 1
}

on_profile() {
	wcli status | grep -qx "wpa_state=COMPLETED" &&
		wcli status | grep -qx "id_str=$1"
}

wpa_status_has() {
	wcli status | grep -qx "$1"
}

associations() {
	grep -c "$WIF: associated with" "$LOG"
}

# The check needs an access point that lies about its channel, a testing
# option of hostapd that distribution builds leave out.
if [ -z "${HOSTAPD_REAL:-}" ]; then
	ok "OCV against an access point that lies about its channel # SKIP without HOSTAPD_REAL"
	summary
	exit
fi
if ! wifi_ready; then
	not_ok "WiFi prerequisites: $HOSTAPD, iw, wpa_cli, $WPA_REAL and mac80211_hwsim"
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
fake run
wifi_profile 10-casa tnd-wpa3 wpa3 "correct horse wpa3"

check "a WPA3 access point with OCV" start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3" 1 "ocv=1"
start_daemon
eventually "the station associates" 20 on_profile 10-casa
check "with SAE" wpa_status_has key_mgmt=SAE
check "and H2E" wpa_status_has sae_h2e=1
check "the network has OCV" net_is 10-casa ocv 1
check "accepted by wpa_supplicant" sh -c "! grep -q 'without ocv' '$LOG'"

stop_ap "$WAP1"
before=$(associations)
check "the same access point, lying about its channel in message 3" start_ap "$WAP1" tnd-wpa3 wpa3 "correct horse wpa3" 1 "ocv=1
oci_freq_override_eapol_m3=2437"
expect_log "wpa\[$WIF\]: .*OCV-FAILURE .*frame=eapol-key-m3" 20 "the station detects it (OCV-FAILURE)"
sleep 2
check "and does not complete the association" sh -c "! wpa_cli -p '$RUN/wpa' -i '$WIF' status | grep -qx wpa_state=COMPLETED"
check "networkd never sees it associated" test "$(associations)" -eq "$before"
stop_daemon
summary
