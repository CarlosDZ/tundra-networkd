#include "proto.h"

#include <arpa/inet.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PROFILE_SET_USAGE                                                      \
	"ERROR usage: PROFILE-SET <profile> <key> <value> [<key> <value>]..."

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
    {"NETWORKS", CMD_NETWORKS, 0, 1, "ERROR usage: NETWORKS [interface]"},
    {"SCAN", CMD_SCAN, 0, 1, "ERROR usage: SCAN [interface]"},
    {"PROFILE", CMD_PROFILE, 1, 1, "ERROR usage: PROFILE <profile>"},
    {"PROFILE-SET", CMD_PROFILE_SET, 3, INT_MAX, PROFILE_SET_USAGE},
    {"PROFILE-DELETE", CMD_PROFILE_DELETE, 1, 1,
     "ERROR usage: PROFILE-DELETE <profile>"},
};

void proto_parse(const char *line, struct cmd *out) {
	char words[4][PROTO_ARG_MAX + 1];
	const char *rest = NULL;
	int n = 0;
	unsigned int too_long = 0;
	memset(out, 0, sizeof(*out));
	for (const char *p = line; *p;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		size_t len = strcspn(p, " \t");
		if (n < 4) {
			if (len > PROTO_ARG_MAX)
				too_long |= 1u << n;
			snprintf(words[n], sizeof(words[n]), "%.*s", (int)len, p);
		}
		n++;
		p += len;
		if (n == 2)
			rest = p;
	}
	if (n == 0) {
		out->type = CMD_EMPTY;
		return;
	}
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strcmp(words[0], commands[i].name) != 0)
			continue;
		int nargs = n - 1, bad = too_long != 0;
		/* The values of PROFILE-SET have their own limit, in proto_draft. */
		if (commands[i].type == CMD_PROFILE_SET)
			bad = (too_long & 2u) != 0;
		if (bad || nargs < commands[i].min || nargs > commands[i].max) {
			out->type = CMD_BAD;
			out->error = commands[i].usage;
			return;
		}
		if (commands[i].type == CMD_PROFILE_SET) {
			nargs = 1;
			out->rest = rest;
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
		return v->unmanaged ? "unmanaged" : v->off ? "off" : "idle";
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

static int safe(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || (c && strchr("._:@,/-", c));
}

/* ADR-025: bytes outside [A-Za-z0-9._:@,/-] go as %XX. A lone "-" too, since
 * that stands for no value. */
int proto_encode(const unsigned char *in, size_t len, char *out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	if (len == 0)
		return -1;
	for (size_t i = 0; i < len; i++) {
		if (safe(in[i]) && !(len == 1 && in[i] == '-')) {
			if (n + 1 >= size)
				return -1;
			out[n++] = (char)in[i];
			continue;
		}
		if (n + 3 >= size)
			return -1;
		out[n++] = '%';
		out[n++] = hex[in[i] >> 4];
		out[n++] = hex[in[i] & 15];
	}
	out[n] = '\0';
	return (int)n;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* The reverse of proto_encode, strict: a byte outside the safe set, a broken
 * escape or %00 are refused. The decoded length, never 0, or -1. */
int proto_decode(const char *in, size_t len, char *out, size_t size) {
	size_t n = 0;
	if (len == 0)
		return -1;
	for (size_t i = 0; i < len; i++) {
		int c = (unsigned char)in[i];
		if (c == '%') {
			int hi = i + 1 < len ? hexval(in[i + 1]) : -1;
			int lo = i + 2 < len ? hexval(in[i + 2]) : -1;
			if (hi < 0 || lo < 0 || (hi | lo) == 0)
				return -1;
			c = hi << 4 | lo;
			i += 2;
		} else if (!safe((unsigned char)c)) {
			return -1;
		}
		if (n + 1 >= size)
			return -1;
		out[n++] = (char)c;
	}
	out[n] = '\0';
	return (int)n;
}

/* The "<key> <value>" pairs of PROFILE-SET. A value "-" names the key
 * without a value, and "*" is taken as it is for the interface, as PROFILE
 * prints it. 0, or -1 with the answer in err. */
int proto_draft(const char *words, struct profile_draft *d, char *err,
                size_t size) {
	char name[16];
	int pairs = 0;
	memset(d, 0, sizeof(*d));
	for (const char *p = words;; pairs++) {
		p += strspn(p, " \t");
		if (!*p)
			break;
		size_t len = strcspn(p, " \t");
		int k = -1;
		if (len < sizeof(name)) {
			memcpy(name, p, len);
			name[len] = '\0';
			k = profile_key(name);
		}
		p += len;
		p += strspn(p, " \t");
		len = strcspn(p, " \t");
		if (k < 0 || d->key[k].named || len == 0) {
			snprintf(err, size, "%s", PROFILE_SET_USAGE);
			return -1;
		}
		struct draft_key *v = &d->key[k];
		v->named = 1;
		if (len == 1 && *p == '*' && k == PKEY_INTERFACE) {
			v->value[0] = '*';
			v->len = 1;
		} else if (!(len == 1 && *p == '-')) {
			int n = proto_decode(p, len, v->value, sizeof(v->value));
			if (n < 0) {
				snprintf(err, size, "ERROR invalid value for %s", name);
				return -1;
			}
			v->len = (size_t)n;
		}
		p += len;
	}
	if (pairs == 0) {
		snprintf(err, size, "%s", PROFILE_SET_USAGE);
		return -1;
	}
	return 0;
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
	char ssid[3 * SSID_MAX + 1] = "-";
	if (v->ssid && proto_encode(v->ssid, v->ssid_len, ssid, sizeof(ssid)) < 0)
		return -1;
	int n = snprintf(
	    out, size,
	    "iface %s state %s%s profile %s address %s trusted %s "
	    "override %s alert %s ssid %s bssid %s%s%s%s",
	    ci->name, state_name(v),
	    ci->st == C_BLOCKED ? block_reason(ci->blocked) : "",
	    v->p ? v->p->name : "-", v->address ? v->address : "-",
	    v->p ? (v->p->trusted && !v->override ? "yes" : "no") : "-", override,
	    v->alerts ? v->alerts : "-", ssid, v->bssid ? v->bssid : "-",
	    note ? " -- " : "", lead, note ? note : "");
	return fit(n, size);
}

static const char *security_name(const struct profile *p) {
	if (!p->has_ssid)
		return "-";
	switch (p->security) {
	case SEC_WPA3:
		return "wpa3";
	case SEC_WPA2:
		return "wpa2";
	case SEC_OPEN:
	case SEC_NONE:
		break;
	}
	return "open";
}

int proto_profile(const struct profile *p, char *out, size_t size) {
	int n;
	if (!p->valid)
		n = snprintf(out, size, "profile %s invalid -- %s", p->name, p->error);
	else {
		char ssid[3 * SSID_MAX + 1] = "-";
		if (p->has_ssid &&
		    proto_encode(p->ssid, p->ssid_len, ssid, sizeof(ssid)) < 0)
			return -1;
		n = snprintf(out, size,
		             "profile %s interface %s autoconnect %s trusted %s "
		             "ssid %s security %s",
		             p->name, p->any_interface ? "*" : p->interface,
		             p->autoconnect ? "yes" : "no", p->trusted ? "yes" : "no",
		             ssid, security_name(p));
	}
	return fit(n, size);
}

/* Every key of a profile but the passphrase, which is only "set" or "-". */
int proto_profile_detail(const struct profile *p, char *out, size_t size) {
	if (!p->valid)
		return proto_profile(p, out, size);
	char iface[3 * IF_NAMESIZE] = "*", ssid[3 * SSID_MAX + 1] = "-";
	char address[INET_ADDRSTRLEN + 4] = "dhcp", gateway[INET_ADDRSTRLEN] = "-";
	char dns[DNS_MAX * INET_ADDRSTRLEN + 1] = "none", mac[3 * MAC_LEN] = "-";
	char ip[INET_ADDRSTRLEN];
	if (!p->any_interface &&
	    proto_encode((const unsigned char *)p->interface, strlen(p->interface),
	                 iface, sizeof(iface)) < 0)
		return -1;
	if (p->has_ssid &&
	    proto_encode(p->ssid, p->ssid_len, ssid, sizeof(ssid)) < 0)
		return -1;
	if (p->address == ADDR_STATIC) {
		inet_ntop(AF_INET, &p->ip, ip, sizeof(ip));
		snprintf(address, sizeof(address), "%s/%d", ip, p->prefix);
	}
	if (p->has_gateway)
		inet_ntop(AF_INET, &p->gateway, gateway, sizeof(gateway));
	if (p->dns == DNS_LEASE)
		snprintf(dns, sizeof(dns), "lease");
	for (int i = 0, n = 0; p->dns == DNS_LIST && i < p->ndns; i++) {
		inet_ntop(AF_INET, &p->dns_list[i], ip, sizeof(ip));
		n += snprintf(dns + n, sizeof(dns) - (size_t)n, "%s%s", i ? "," : "",
		              ip);
	}
	if (p->mac.mode == MAC_KEEP)
		snprintf(mac, sizeof(mac), "keep");
	else if (p->mac.mode == MAC_RANDOM)
		snprintf(mac, sizeof(mac), "random");
	else if (p->mac.mode == MAC_FIXED)
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
		         p->mac.addr[0], p->mac.addr[1], p->mac.addr[2], p->mac.addr[3],
		         p->mac.addr[4], p->mac.addr[5]);
	int n =
	    snprintf(out, size,
	             "profile %s interface %s ssid %s security %s "
	             "passphrase %s address %s gateway %s dns %s "
	             "autoconnect %s trusted %s mac %s",
	             p->name, iface, ssid, security_name(p),
	             p->has_passphrase ? "set" : "-", address, gateway, dns,
	             p->autoconnect ? "yes" : "no", p->trusted ? "yes" : "no", mac);
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

int proto_alert_closed(unsigned int id, const char *how, char *out,
                       size_t size) {
	return fit(snprintf(out, size, "event alert-closed %u %s", id, how), size);
}

static const char *bss_security(enum wpa_sec sec) {
	switch (sec) {
	case WSEC_OPEN:
		break;
	case WSEC_OWE:
		return "owe";
	case WSEC_WEP:
		return "wep";
	case WSEC_WPA:
		return "wpa";
	case WSEC_WPA2:
		return "wpa2";
	case WSEC_WPA2_WPA3:
		return "wpa2/wpa3";
	case WSEC_WPA3:
		return "wpa3";
	case WSEC_ENTERPRISE:
		return "enterprise";
	}
	return "open";
}

int proto_scan(const char *ifname, long age_s, char *out, size_t size) {
	if (age_s < 0)
		return fit(snprintf(out, size, "scan %s age -", ifname), size);
	return fit(snprintf(out, size, "scan %s age %ld", ifname, age_s), size);
}

int proto_network(const char *ifname, const struct wpa_bss *b,
                  const char *profile, char *out, size_t size) {
	char ssid[3 * SSID_MAX + 1] = "-";
	if (b->ssid_len &&
	    proto_encode(b->ssid, b->ssid_len, ssid, sizeof(ssid)) < 0)
		return -1;
	return fit(snprintf(out, size,
	                    "network %s ssid %s bssid %s freq %d signal %d "
	                    "security %s profile %s",
	                    ifname, ssid, b->bssid, b->freq, b->signal,
	                    bss_security(b->sec), profile ? profile : "-"),
	           size);
}

int proto_scan_event(const char *ifname, char *out, size_t size) {
	return fit(snprintf(out, size, "event scan %s", ifname), size);
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
	case CONNECT_OFF:
		return "ERROR wifi is off";
	}
	return "ERROR unknown error";
}
