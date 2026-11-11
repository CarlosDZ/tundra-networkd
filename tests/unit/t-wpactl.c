#include "test.h"
#include "wpactl.c"

static struct wpa_event ev;
static struct wpa_status st;

static int event(const char *msg) {
	return wpactl_event(msg, strlen(msg), &ev);
}

static int status(const char *text) {
	return wpactl_status(text, strlen(text), &st);
}

static int decoded(const char *in, const char *want, size_t want_len) {
	unsigned char out[32];
	size_t n;
	return wpactl_decode(in, strlen(in), out, sizeof(out), &n) == 0 &&
	       n == want_len && memcmp(out, want, n) == 0;
}

static struct wpa_network net;

static struct profile wifi(const char *name, const char *ssid,
                           enum security sec, const char *pass) {
	struct profile p;
	memset(&p, 0, sizeof(p));
	snprintf(p.name, sizeof(p.name), "%s", name);
	p.ssid_len = strlen(ssid);
	memcpy(p.ssid, ssid, p.ssid_len);
	p.has_ssid = 1;
	p.security = sec;
	snprintf(p.passphrase, sizeof(p.passphrase), "%s", pass);
	return p;
}

static int vars_are(const char *const *want, int n) {
	if (net.n != n)
		return 0;
	for (int i = 0; i < n; i++)
		if (strcmp(net.vars[i], want[i]) != 0)
			return 0;
	return 1;
}

static int last_is(const char *var) {
	return net.n > 0 && strcmp(net.vars[net.n - 1], var) == 0;
}

static int scanned(const char *text) {
	struct wpa_bss one[2];
	return wpactl_scan_results(text, strlen(text), one, 2);
}

int main(void) {
	CHECK(event("<3>CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:01:00 "
	            "completed [id=0 id_str=casa]") == 0 &&
	          ev.type == WPA_EV_CONNECTED &&
	          strcmp(ev.bssid, "02:00:00:00:01:00") == 0 && ev.id == 0 &&
	          strcmp(ev.id_str, "casa") == 0,
	      "a connection event");
	CHECK(event("<3>CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:01:00 "
	            "completed [id=4 id_str=]") == 0 &&
	          ev.id == 4 && ev.id_str[0] == '\0',
	      "a network without id_str");
	CHECK(event("<3>CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:01:00 reason=3 "
	            "locally_generated=1") == 0 &&
	          ev.type == WPA_EV_DISCONNECTED &&
	          strcmp(ev.bssid, "02:00:00:00:01:00") == 0 && ev.reason == 3 &&
	          ev.id == -1,
	      "a disconnection event");
	CHECK(event("<4>CTRL-EVENT-SSID-TEMP-DISABLED id=1 ssid=\"casa\" "
	            "auth_failures=1 duration=10 reason=WRONG_KEY") == 0 &&
	          ev.type == WPA_EV_TEMP_DISABLED && ev.id == 1,
	      "a network disabled after a wrong key");
	CHECK(event("<2>CTRL-EVENT-TERMINATING") == 0 &&
	          ev.type == WPA_EV_TERMINATING,
	      "wpa_supplicant terminating");
	CHECK(event("<3>CTRL-EVENT-BSS-ADDED 0 02:00:00:00:01:00") == 0 &&
	          ev.type == WPA_EV_OTHER,
	      "an event that does not matter");
	CHECK(event("OK") < 0 && event("") < 0 && event("<3") < 0,
	      "a reply is not an event");

	CHECK(status("bssid=02:00:00:00:01:00\nfreq=2412\nssid=tnd caf\\xc3\\xa9\n"
	             "id=2\nid_str=cafe\nmode=station\nwpa_state=COMPLETED\n"
	             "address=02:00:00:00:02:00\n") == 0 &&
	          strcmp(st.state, "COMPLETED") == 0 &&
	          strcmp(st.bssid, "02:00:00:00:01:00") == 0 && st.id == 2 &&
	          strcmp(st.id_str, "cafe") == 0 && st.ssid_len == 9 &&
	          memcmp(st.ssid, "tnd caf\xc3\xa9", 9) == 0,
	      "STATUS of a connected interface");
	CHECK(status("wpa_state=SCANNING\naddress=02:00:00:00:02:00\n") == 0 &&
	          strcmp(st.state, "SCANNING") == 0 && st.id == -1 &&
	          st.ssid_len == 0 && st.bssid[0] == '\0',
	      "STATUS while scanning");
	CHECK(status("FAIL\n") < 0, "a reply without wpa_state");

	CHECK(decoded("casa", "casa", 4), "a plain SSID");
	CHECK(decoded("a\\\"b\\\\c", "a\"b\\c", 5), "quotes and backslashes");
	CHECK(decoded("\\e\\n\\r\\t", "\033\n\r\t", 4), "control escapes");
	CHECK(decoded("\\x00\\xff", "\0\xff", 2), "a NUL and a high byte");
	unsigned char out[4];
	size_t n;
	CHECK(wpactl_decode("\\x4", 3, out, sizeof(out), &n) < 0 &&
	          wpactl_decode("\\q", 2, out, sizeof(out), &n) < 0 &&
	          wpactl_decode("abc\\", 4, out, sizeof(out), &n) < 0 &&
	          wpactl_decode("abcde", 5, out, sizeof(out), &n) < 0,
	      "broken escapes and an SSID too long are refused");

	char hex[2 * 32 + 1];
	wpactl_hex((const unsigned char *)"tnd \xff", 5, hex);
	CHECK(strcmp(hex, "746e6420ff") == 0, "an SSID in hexadecimal");
	wpactl_hex((const unsigned char *)"", 0, hex);
	CHECK(hex[0] == '\0', "an empty SSID");
	struct profile p = wifi("10-casa", "casa", SEC_WPA3, "correct \"horse\"");
	wpactl_network(&p, MAC_KEEP, NULL, &net);
	static const char *const wpa3[] = {
	    "ssid 63617361",
	    "id_str \"10-casa\"",
	    "key_mgmt SAE",
	    "proto RSN",
	    "pairwise CCMP",
	    "ieee80211w 2",
	    "sae_password 636f72726563742022686f72736522",
	    "ocv 1",
	    "mac_addr 0"};
	CHECK(vars_are(wpa3, 9), "wpa3: SAE, PMF required and the password in hex");
	CHECK(net.optional[7] && !net.optional[4], "only ocv is optional");
	uint64_t h = net.hash;
	wpactl_network(&p, MAC_KEEP, NULL, &net);
	CHECK(net.hash == h, "the same profile, the same hash");
	snprintf(p.passphrase, sizeof(p.passphrase), "another one");
	wpactl_network(&p, MAC_KEEP, NULL, &net);
	CHECK(net.hash != h, "a new passphrase changes it");

	p = wifi("20-cafe", "caf\xc3\xa9 1", SEC_WPA2, "correct horse");
	wpactl_network(&p, MAC_KEEP, NULL, &net);
	static const char *const wpa2[] = {
	    "ssid 636166c3a92031",   "id_str \"20-cafe\"",
	    "key_mgmt WPA-PSK",      "proto RSN",
	    "pairwise CCMP",         "ieee80211w 1",
	    "psk \"correct horse\"", "mac_addr 0"};
	CHECK(vars_are(wpa2, 8), "wpa2: WPA-PSK, optional PMF, quoted passphrase");

	p = wifi("30-bar", "bar", SEC_OPEN, "");
	wpactl_network(&p, MAC_KEEP, NULL, &net);
	static const char *const open[] = {"ssid 626172", "id_str \"30-bar\"",
	                                   "key_mgmt NONE", "mac_addr 0"};
	CHECK(vars_are(open, 4), "open: no key");

	static const unsigned char hw[MAC_LEN] = {0x52, 0x54, 0x00,
	                                          0x12, 0x34, 0x56};
	h = net.hash;
	CHECK(last_is("mac_addr 0"), "no mac anywhere keeps the hardware address");
	wpactl_network(&p, MAC_RANDOM, hw, &net);
	CHECK(last_is("mac_addr 1") && net.hash != h,
	      "a random base makes the network random");
	wpactl_network(&p, MAC_FIXED, hw, &net);
	CHECK(last_is("mac_addr 0"),
	      "a fixed base is already the address of the interface");
	p.mac.mode = MAC_RANDOM;
	wpactl_network(&p, MAC_KEEP, hw, &net);
	CHECK(last_is("mac_addr 1"), "mac = random in the profile");
	p.mac =
	    (struct mac_policy){MAC_FIXED, {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a}};
	wpactl_network(&p, MAC_RANDOM, hw, &net);
	CHECK(last_is("mac_addr 3") &&
	          strcmp(net.vars[net.n - 2], "mac_value 02:12:34:56:78:9a") == 0,
	      "a fixed mac in the profile, its value set first");
	p.mac.mode = MAC_KEEP;
	wpactl_network(&p, MAC_KEEP, hw, &net);
	CHECK(last_is("mac_addr 0"), "mac = keep over a base that keeps");
	wpactl_network(&p, MAC_RANDOM, hw, &net);
	CHECK(last_is("mac_addr 3") &&
	          strcmp(net.vars[net.n - 2], "mac_value 52:54:00:12:34:56") == 0,
	      "mac = keep over another base asks for the hardware address");
	wpactl_network(&p, MAC_RANDOM, NULL, &net);
	CHECK(last_is("mac_addr 0"),
	      "without a known hardware address, the one of the interface");
	CHECK(event("<3>CTRL-EVENT-SCAN-RESULTS ") == 0 &&
	          ev.type == WPA_EV_SCAN_RESULTS,
	      "a scan results event");
	CHECK(event("<3>CTRL-EVENT-SCAN-STARTED ") == 0 && ev.type == WPA_EV_OTHER,
	      "the start of a scan is not its results");

	static struct wpa_bss b[4];
	const char *scan =
	    "bssid / frequency / signal level / flags / ssid\n"
	    "02:00:00:00:01:00\t2412\t-30\t[WPA2-SAE-CCMP][ESS]\ttnd-wpa3\n"
	    "02:00:00:00:02:00\t5180\t-71\t[WPA2-PSK+SAE-CCMP][ESS]\tcaf\\xc3\\xa9 "
	    "libre\n"
	    "02:00:00:00:03:00\t2437\t-50\t[ESS]\t\n"
	    "not a line\n"
	    "02:00:00:00:04:00\t2462\t-60\t[WPA2-EAP-CCMP][ESS]\tcorp\n"
	    "02:00:00:00:05:00\t2462\t-60\t[WEP][ESS]\told\n";
	int found = wpactl_scan_results(scan, strlen(scan), b, 4);
	CHECK(found == 4,
	      "the header and broken lines are skipped, the rest capped");
	CHECK(strcmp(b[0].bssid, "02:00:00:00:01:00") == 0 && b[0].freq == 2412 &&
	          b[0].signal == -30 && b[0].sec == WSEC_WPA3 &&
	          b[0].ssid_len == 8 && memcmp(b[0].ssid, "tnd-wpa3", 8) == 0,
	      "an access point with every field");
	CHECK(b[1].sec == WSEC_WPA2_WPA3 && b[1].ssid_len == 11 &&
	          memcmp(b[1].ssid, "caf\xc3\xa9 libre", 11) == 0,
	      "transition mode and an escaped ssid");
	CHECK(b[2].sec == WSEC_OPEN && b[2].ssid_len == 0,
	      "an open hidden network");
	CHECK(b[3].sec == WSEC_ENTERPRISE, "an enterprise network");

	const char *more =
	    "02:00:00:00:06:00\t2412\t-30\t[WPA-PSK-TKIP][ESS]\ta\n"
	    "02:00:00:00:07:00\t2412\t-30\t[WPA2-PSK-CCMP][ESS]\tb\n"
	    "02:00:00:00:08:00\t2412\t-30\t[OWE-TRANS][ESS]\tc\n"
	    "02:00:00:00:09:00\t2412\t-30\t[WEP][ESS]\t\\x00\\x00\\x00";
	found = wpactl_scan_results(more, strlen(more), b, 4);
	CHECK(found == 4 && b[0].sec == WSEC_WPA && b[1].sec == WSEC_WPA2 &&
	          b[2].sec == WSEC_OWE && b[3].sec == WSEC_WEP,
	      "wpa, wpa2, owe and wep");
	CHECK(b[3].ssid_len == 0, "an ssid of zeros is a hidden network");
	CHECK(scanned("02:00:00:00:01:0g\t1\t1\t[ESS]\tx\n") == 0 &&
	          scanned("02:00:00:00:01:00\tx\t1\t[ESS]\tx\n") == 0 &&
	          scanned("02:00:00:00:01:00\t1\t1\n") == 0 &&
	          scanned("02:00:00:00:01:00\t1\t1\t[ESS]\t\\q\n") == 0,
	      "a bad bssid, a bad number, a short line and a bad escape");
	CHECK(scanned("02:00:00:00:01:00\t1\t1\t[ESS]") == 1 && scanned("") == 0,
	      "a line without ssid or newline, and no lines");
	return test_summary();
}
