#ifndef TUNDRA_WPACTL_H
#define TUNDRA_WPACTL_H

#include "profile.h"

#include <stddef.h>
#include <stdint.h>

#define WPA_BSSID_MAX 18
#define WPA_STATE_MAX 32
#define WPA_VARS_MAX 8
#define WPA_SCAN_MAX 48
#define WPA_VAR_MAX (16 + 2 * PASSPHRASE_MAX + 1)

enum wpa_event_type {
	WPA_EV_OTHER,
	WPA_EV_CONNECTED,
	WPA_EV_DISCONNECTED,
	WPA_EV_TEMP_DISABLED,
	WPA_EV_TERMINATING,
	WPA_EV_SCAN_RESULTS
};

struct wpa_event {
	enum wpa_event_type type;
	char bssid[WPA_BSSID_MAX];
	int id;
	char id_str[PROFILE_NAME_MAX + 1];
	int reason;
};

struct wpa_status {
	char state[WPA_STATE_MAX];
	char bssid[WPA_BSSID_MAX];
	unsigned char ssid[SSID_MAX];
	size_t ssid_len;
	int id;
	char id_str[PROFILE_NAME_MAX + 1];
};

enum wpa_sec {
	WSEC_OPEN,
	WSEC_OWE,
	WSEC_WEP,
	WSEC_WPA,
	WSEC_WPA2,
	WSEC_WPA2_WPA3,
	WSEC_WPA3,
	WSEC_ENTERPRISE
};

/* An access point of the last scan; ssid_len 0 is a hidden network. */
struct wpa_bss {
	char bssid[WPA_BSSID_MAX];
	int freq;
	int signal;
	enum wpa_sec sec;
	unsigned char ssid[SSID_MAX];
	size_t ssid_len;
};

struct wpa_network {
	char vars[WPA_VARS_MAX][WPA_VAR_MAX];
	int optional[WPA_VARS_MAX];
	int n;
	uint64_t hash;
};

int wpactl_event(const char *msg, size_t len, struct wpa_event *ev);
int wpactl_status(const char *text, size_t len, struct wpa_status *st);
int wpactl_decode(const char *in, size_t len, unsigned char *out, size_t max,
                  size_t *outlen);
int wpactl_scan_results(const char *text, size_t len, struct wpa_bss *out,
                        int max);
void wpactl_hex(const unsigned char *in, size_t len, char *out);
void wpactl_network(const struct profile *p, struct wpa_network *w);

#endif
