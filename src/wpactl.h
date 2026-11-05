#ifndef TUNDRA_WPACTL_H
#define TUNDRA_WPACTL_H

#include "profile.h"

#include <stddef.h>

#define WPA_BSSID_MAX 18
#define WPA_STATE_MAX 32

enum wpa_event_type {
	WPA_EV_OTHER,
	WPA_EV_CONNECTED,
	WPA_EV_DISCONNECTED,
	WPA_EV_TEMP_DISABLED,
	WPA_EV_TERMINATING
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

int wpactl_event(const char *msg, size_t len, struct wpa_event *ev);
int wpactl_status(const char *text, size_t len, struct wpa_status *st);
int wpactl_decode(const char *in, size_t len, unsigned char *out, size_t max,
                  size_t *outlen);
void wpactl_hex(const unsigned char *in, size_t len, char *out);

#endif
