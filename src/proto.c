#include "proto.h"

#include <stdio.h>
#include <string.h>

static const struct {
	const char *name;
	enum cmd_type type;
	int min, max;
	const char *usage;
} commands[] = {
    {"STATUS", CMD_STATUS, 0, 0, "ERROR usage: STATUS"},
    {"PROFILES", CMD_PROFILES, 0, 0, "ERROR usage: PROFILES"},
    {"CONNECT", CMD_CONNECT, 1, 2,
     "ERROR usage: CONNECT <profile> [interface]"},
    {"DISCONNECT", CMD_DISCONNECT, 1, 1, "ERROR usage: DISCONNECT <interface>"},
    {"RELOAD", CMD_RELOAD, 0, 0, "ERROR usage: RELOAD"},
    {"SUBSCRIBE", CMD_SUBSCRIBE, 0, 0, "ERROR usage: SUBSCRIBE"},
    {"ALERTS", CMD_ALERTS, 0, 0, "ERROR usage: ALERTS"},
    {"ALERT-REPLY", CMD_ALERT_REPLY, 2, 2,
     "ERROR usage: ALERT-REPLY <id> <option>"},
};

void proto_parse(const char *line, struct cmd *out) {
	char words[4][PROTO_ARG_MAX + 1];
	int n = 0, too_long = 0;
	memset(out, 0, sizeof(*out));
	for (const char *p = line; *p;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		size_t len = strcspn(p, " \t");
		if (n < 4) {
			too_long |= len > PROTO_ARG_MAX;
			snprintf(words[n], sizeof(words[n]), "%.*s", (int)len, p);
		}
		n++;
		p += len;
	}
	if (n == 0) {
		out->type = CMD_EMPTY;
		return;
	}
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strcmp(words[0], commands[i].name) != 0)
			continue;
		int nargs = n - 1;
		if (too_long || nargs < commands[i].min || nargs > commands[i].max) {
			out->type = CMD_BAD;
			out->error = commands[i].usage;
			return;
		}
		out->type = commands[i].type;
		out->nargs = nargs;
		for (int a = 0; a < nargs; a++)
			memcpy(out->args[a], words[a + 1], sizeof(out->args[a]));
		return;
	}
	out->type = CMD_BAD;
	out->error = "ERROR unknown command";
}

static const char *state_name(const struct iface_view *v) {
	switch (v->ci->st) {
	case C_IDLE:
		return v->unmanaged ? "unmanaged" : "idle";
	case C_RUNNING:
		return v->connected ? "connected" : "connecting";
	case C_PROBING:
		return "connecting";
	case C_STATIC:
		return "connected";
	case C_STOPPING:
		return "stopping";
	case C_BACKOFF:
		return "retrying";
	case C_WAIT_LINK:
		return "waiting";
	case C_BLOCKED:
		return "blocked";
	}
	return "idle";
}

static const char *block_reason(enum block_reason b) {
	switch (b) {
	case B_CONFIG:
		return " reason config";
	case B_EXITED:
		return " reason exited";
	case B_ALERT:
		return " reason alert";
	case B_TAKEN:
		return " reason taken";
	case B_NONE:
		break;
	}
	return "";
}

static int fit(int n, size_t size) {
	return n < 0 || (size_t)n >= size ? -1 : n;
}

int proto_status(const struct iface_view *v, char *out, size_t size) {
	const struct core_iface *ci = v->ci;
	const char *lead = "", *note = NULL;
	if (ci->st == C_IDLE && v->unmanaged) {
		note = v->unmanaged;
	} else if (v->kept && v->kept[0]) {
		lead = "previous configuration kept: ";
		note = v->kept;
	}
	char override[16] = "-";
	if (v->p && v->override)
		snprintf(override, sizeof(override), "%u", v->override);
	int n =
	    snprintf(out, size,
	             "iface %s state %s%s profile %s address %s trusted %s "
	             "override %s alert %s%s%s%s",
	             ci->name, state_name(v),
	             ci->st == C_BLOCKED ? block_reason(ci->blocked) : "",
	             v->p ? v->p->name : "-", v->address ? v->address : "-",
	             v->p ? (v->p->trusted && !v->override ? "yes" : "no") : "-",
	             override, v->alerts ? v->alerts : "-", note ? " -- " : "",
	             lead, note ? note : "");
	return fit(n, size);
}

int proto_profile(const struct profile *p, char *out, size_t size) {
	int n;
	if (!p->valid)
		n = snprintf(out, size, "profile %s invalid -- %s", p->name, p->error);
	else
		n = snprintf(out, size,
		             "profile %s interface %s autoconnect %s trusted %s",
		             p->name, p->any_interface ? "*" : p->interface,
		             p->autoconnect ? "yes" : "no", p->trusted ? "yes" : "no");
	return fit(n, size);
}

int proto_event(int connected, const char *ifname, const char *profile,
                const char *address, char *out, size_t size) {
	int n;
	if (connected)
		n = snprintf(out, size, "event connected %s %s address %s", ifname,
		             profile, address);
	else
		n = snprintf(out, size, "event disconnected %s %s", ifname, profile);
	return fit(n, size);
}

const char *proto_connect_error(enum connect_result r) {
	switch (r) {
	case CONNECT_OK:
		return NULL;
	case CONNECT_NO_PROFILE:
		return "ERROR unknown profile";
	case CONNECT_INVALID:
		return "ERROR profile has errors";
	case CONNECT_NO_MATCH:
		return "ERROR profile does not match";
	case CONNECT_UNMANAGED:
		return "ERROR interface not managed";
	case CONNECT_NO_IFACE:
		return "ERROR no such interface";
	case CONNECT_BLOCKED:
		return "ERROR profile rejected by tundra-dhcpc, reload first";
	case CONNECT_ALERT_PENDING:
		return "ERROR alert pending, answer it first";
	}
	return "ERROR unknown error";
}
