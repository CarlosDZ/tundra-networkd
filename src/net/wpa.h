#ifndef TUNDRA_WPA_H
#define TUNDRA_WPA_H

#include <net/if.h>
#include <stddef.h>

#define WPA_WATCH_MAX 16
#define WPA_TIMEOUT_MS 2000

struct wpa_conn {
	int ctl;
	int mon;
};

int wpa_watch_open(const char *dir);
int wpa_watch_read(int fd, char names[][IF_NAMESIZE], int max);
int wpa_connect(const char *dir, const char *ifname, struct wpa_conn *w);
int wpa_request(int fd, const char *cmd, char *reply, size_t size);
int wpa_event_read(int fd, char *buf, size_t size);
void wpa_close(struct wpa_conn *w);

#endif
