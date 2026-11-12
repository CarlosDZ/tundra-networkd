#ifndef TUNDRA_PROTO_H
#define TUNDRA_PROTO_H

#include "core.h"
#include "profile.h"
#include "wpactl.h"

#include <stddef.h>

#define PROTO_ARG_MAX 64
#define PROTO_ERR_MAX 96

enum cmd_type {
	CMD_EMPTY,
	CMD_STATUS,
	CMD_PROFILES,
	CMD_CONNECT,
	CMD_DISCONNECT,
	CMD_RELOAD,
	CMD_SUBSCRIBE,
	CMD_ALERTS,
	CMD_ALERT_REPLY,
	CMD_NETWORKS,
	CMD_SCAN,
	CMD_PROFILE,
	CMD_PROFILE_SET,
	CMD_PROFILE_DELETE,
	CMD_BAD
};

struct cmd {
	enum cmd_type type;
	int nargs;
	char args[2][PROTO_ARG_MAX + 1];
	/* PROFILE-SET: what follows the name, inside the parsed line. */
	const char *rest;
	const char *error;
};

struct iface_view {
	const struct core_iface *ci;
	const struct profile *p;
	int connected;
	const char *address;
	const char *unmanaged;
	const char *kept;
	unsigned int override;
	const char *alerts;
	int off;
	const unsigned char *ssid;
	size_t ssid_len;
	const char *bssid;
};

void proto_parse(const char *line, struct cmd *out);
int proto_status(const struct iface_view *v, char *out, size_t size);
int proto_profile(const struct profile *p, char *out, size_t size);
int proto_event(int connected, const char *ifname, const char *profile,
                const char *address, char *out, size_t size);
int proto_alert_closed(unsigned int id, const char *how, char *out,
                       size_t size);
int proto_scan(const char *ifname, long age_s, char *out, size_t size);
int proto_network(const char *ifname, const struct wpa_bss *b,
                  const char *profile, char *out, size_t size);
int proto_scan_event(const char *ifname, char *out, size_t size);
const char *proto_connect_error(enum connect_result r);
int proto_encode(const unsigned char *in, size_t len, char *out, size_t size);
int proto_decode(const char *in, size_t len, char *out, size_t size);
int proto_draft(const char *words, struct profile_draft *d, char *err,
                size_t size);
int proto_profile_detail(const struct profile *p, char *out, size_t size);

#endif
