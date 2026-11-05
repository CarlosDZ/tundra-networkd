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
	CHECK(event("<3>CTRL-EVENT-SCAN-RESULTS ") == 0 && ev.type == WPA_EV_OTHER,
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
	return test_summary();
}
