#ifndef TUNDRA_PROTO_H
#define TUNDRA_PROTO_H

#include "core.h"
#include "profile.h"

#include <stddef.h>

#define PROTO_ARG_MAX 64

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
	CMD_BAD
};

struct cmd {
	enum cmd_type type;
	int nargs;
	char args[2][PROTO_ARG_MAX + 1];
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
const char *proto_connect_error(enum connect_result r);
int proto_encode(const unsigned char *in, size_t len, char *out, size_t size);

#endif
