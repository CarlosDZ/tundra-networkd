#include "profile.h"
#include "grammar.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum section { S_MATCH, S_APPLY };
enum kind { K_TOKEN, K_TEXT };

enum {
	KEY_INTERFACE,
	KEY_SSID,
	KEY_SECURITY,
	KEY_PASSPHRASE,
	KEY_ADDRESS,
	KEY_GATEWAY,
	KEY_DNS,
	KEY_AUTOCONNECT,
	KEY_TRUSTED,
	KEY_COUNT
};

static const struct {
	const char *name;
	enum section sec;
	enum kind kind;
} keys[KEY_COUNT] = {
    [KEY_INTERFACE] = {"interface", S_MATCH, K_TOKEN},
    [KEY_SSID] = {"ssid", S_MATCH, K_TEXT},
    [KEY_SECURITY] = {"security", S_MATCH, K_TOKEN},
    [KEY_PASSPHRASE] = {"passphrase", S_APPLY, K_TEXT},
    [KEY_ADDRESS] = {"address", S_APPLY, K_TOKEN},
    [KEY_GATEWAY] = {"gateway", S_APPLY, K_TOKEN},
    [KEY_DNS] = {"dns", S_APPLY, K_TOKEN},
    [KEY_AUTOCONNECT] = {"autoconnect", S_APPLY, K_TOKEN},
    [KEY_TRUSTED] = {"trusted", S_APPLY, K_TOKEN},
};

static const char *section_name(enum section s) {
	return s == S_MATCH ? "match" : "apply";
}

static int pr_fail(struct profile *p, int line, const char *fmt, ...) {
	char msg[CONF_ERR_MAX - 16];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (line > 0)
		snprintf(p->error, sizeof(p->error), "line %d: %.100s", line, msg);
	else
		snprintf(p->error, sizeof(p->error), "%s", msg);
	p->valid = 0;
	return -1;
}

int conf_valid_ifname(const char *s) {
	size_t n = strlen(s);
	if (n == 0 || n >= IF_NAMESIZE || strcmp(s, ".") == 0 ||
	    strcmp(s, "..") == 0)
		return 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '/' || s[i] == ':' || s[i] == ' ' || s[i] == '\t')
			return 0;
	return 1;
}

int profile_name_from_file(const char *file, char name[PROFILE_NAME_MAX + 1]) {
	size_t n = strlen(file);
	if (n <= 4 || strcmp(file + n - 4, ".tnc") != 0)
		return -1;
	n -= 4;
	if (n > PROFILE_NAME_MAX || file[0] == '.')
		return -2;
	for (size_t i = 0; i < n; i++) {
		char c = file[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return -2;
	}
	memcpy(name, file, n);
	name[n] = '\0';
	return 0;
}

static int parse_ip(const char *s, uint32_t *out) {
	struct in_addr a;
	if (inet_pton(AF_INET, s, &a) != 1 || a.s_addr == 0)
		return -1;
	*out = a.s_addr;
	return 0;
}

static int parse_prefix(const char *s, int *out) {
	if (s[0] < '1' || s[0] > '9' || strlen(s) > 2)
		return -1;
	int v = 0;
	for (const char *c = s; *c; c++) {
		if (*c < '0' || *c > '9')
			return -1;
		v = v * 10 + (*c - '0');
	}
	if (v > 32)
		return -1;
	*out = v;
	return 0;
}

static uint32_t prefix_mask(int prefix) {
	return htonl(prefix == 0 ? 0 : 0xffffffffu << (32 - prefix));
}

static int parse_address(struct profile *p, const struct gr_item *it) {
	if (strcmp(it->value, "dhcp") == 0) {
		p->address = ADDR_DHCP;
		return 0;
	}
	char ip[GR_VALUE_MAX];
	snprintf(ip, sizeof(ip), "%s", it->value);
	char *slash = strchr(ip, '/');
	if (!slash)
		return pr_fail(p, it->line, "address needs a prefix: %s", it->value);
	*slash = '\0';
	if (parse_ip(ip, &p->ip) < 0 || parse_prefix(slash + 1, &p->prefix) < 0)
		return pr_fail(p, it->line, "invalid address: %s", it->value);
	if (p->prefix <= 30) {
		uint32_t host = p->ip & ~prefix_mask(p->prefix);
		if (host == 0 || host == ~prefix_mask(p->prefix))
			return pr_fail(p, it->line,
			               "address is the network or broadcast address: %s",
			               it->value);
	}
	p->address = ADDR_STATIC;
	return 0;
}

static int parse_gateway(struct profile *p, const struct gr_item *it) {
	if (p->address != ADDR_STATIC)
		return pr_fail(p, it->line, "gateway requires a static address");
	if (parse_ip(it->value, &p->gateway) < 0)
		return pr_fail(p, it->line, "invalid gateway: %s", it->value);
	uint32_t mask = prefix_mask(p->prefix);
	if ((p->gateway & mask) != (p->ip & mask) || p->gateway == p->ip)
		return pr_fail(p, it->line, "gateway outside the address network: %s",
		               it->value);
	p->has_gateway = 1;
	return 0;
}

static int parse_dns(struct profile *p, const struct gr_item *it) {
	if (strcmp(it->value, "lease") == 0) {
		if (p->address != ADDR_DHCP)
			return pr_fail(p, it->line, "dns = lease requires address = dhcp");
		p->dns = DNS_LEASE;
		return 0;
	}
	if (strcmp(it->value, "none") == 0) {
		p->dns = DNS_NONE;
		return 0;
	}

	char buf[GR_VALUE_MAX];
	snprintf(buf, sizeof(buf), "%s", it->value);
	char *s = buf;
	p->dns = DNS_LIST;
	for (;;) {
		char *comma = strchr(s, ',');
		if (comma)
			*comma = '\0';
		if (p->ndns == DNS_MAX)
			return pr_fail(p, it->line, "more than %d dns servers", DNS_MAX);
		if (parse_ip(s, &p->dns_list[p->ndns]) < 0)
			return pr_fail(p, it->line, "invalid dns: %s", it->value);
		p->ndns++;
		if (!comma)
			return 0;
		s = comma + 1;
		while (*s == ' ' || *s == '\t')
			s++;
	}
}

static int parse_bool(struct profile *p, const struct gr_item *it, int *out) {
	if (strcmp(it->value, "yes") == 0)
		*out = 1;
	else if (strcmp(it->value, "no") == 0)
		*out = 0;
	else
		return pr_fail(p, it->line, "%s must be yes or no", it->key);
	return 0;
}

static int parse_passphrase(struct profile *p, const struct gr_item *it) {
	if (p->security == SEC_WPA2) {
		if (it->len < 8 || it->len > 63)
			return pr_fail(p, it->line,
			               "wpa2 passphrase must have 8 to 63 "
			               "characters");
		for (size_t i = 0; i < it->len; i++)
			if ((unsigned char)it->value[i] < 0x20 ||
			    (unsigned char)it->value[i] > 0x7e)
				return pr_fail(p, it->line,
				               "wpa2 passphrase must be printable ASCII");
	} else if (it->len > PASSPHRASE_MAX) {
		return pr_fail(p, it->line, "passphrase too long");
	}
	memcpy(p->passphrase, it->value, it->len);
	p->passphrase[it->len] = '\0';
	p->has_passphrase = 1;
	return 0;
}

static int find_key(const char *name) {
	for (int k = 0; k < KEY_COUNT; k++)
		if (strcmp(keys[k].name, name) == 0)
			return k;
	return -1;
}

static int check_structure(struct profile *p, const struct grammar *g,
                           const struct gr_item *seen[KEY_COUNT]) {
	int block_sec[GR_BLOCKS_MAX];
	int have[2] = {0, 0};

	for (int b = 0; b < g->nblocks; b++) {
		const char *n = g->blocks[b].name;
		if (strcmp(n, "match") == 0)
			block_sec[b] = S_MATCH;
		else if (strcmp(n, "apply") == 0)
			block_sec[b] = S_APPLY;
		else
			return pr_fail(p, g->blocks[b].line, "unknown section %s", n);
		if (have[block_sec[b]]++)
			return pr_fail(p, g->blocks[b].line, "repeated section %s", n);
	}
	if (!have[S_MATCH])
		return pr_fail(p, 0, "missing match section");
	if (!have[S_APPLY])
		return pr_fail(p, 0, "missing apply section");

	for (int i = 0; i < g->n; i++) {
		const struct gr_item *it = &g->items[i];
		if (it->block < 0)
			return pr_fail(p, it->line, "%s outside a section", it->key);
		int k = find_key(it->key);
		if (k < 0)
			return pr_fail(p, it->line, "unknown key %s", it->key);
		if ((int)keys[k].sec != block_sec[it->block])
			return pr_fail(p, it->line, "%s belongs in %s", it->key,
			               section_name(keys[k].sec));
		if (seen[k])
			return pr_fail(p, it->line, "repeated key %s", it->key);
		if (keys[k].kind == K_TEXT && !it->quoted)
			return pr_fail(p, it->line, "%s must be quoted", it->key);
		if (keys[k].kind == K_TOKEN && it->quoted)
			return pr_fail(p, it->line, "%s must not be quoted", it->key);
		seen[k] = it;
	}
	return 0;
}

static const char *missing(const struct gr_item *seen[KEY_COUNT]) {
	static const int required[] = {KEY_INTERFACE, KEY_ADDRESS, KEY_DNS,
	                               KEY_AUTOCONNECT, KEY_TRUSTED};
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
		if (!seen[required[i]])
			return keys[required[i]].name;
	return NULL;
}

static int check_values(struct profile *p,
                        const struct gr_item *seen[KEY_COUNT]) {
	const struct gr_item *it;

	const char *absent = missing(seen);
	if (absent)
		return pr_fail(p, 0, "missing %s", absent);

	it = seen[KEY_INTERFACE];
	if (strcmp(it->value, "*") == 0)
		p->any_interface = 1;
	else if (conf_valid_ifname(it->value))
		snprintf(p->interface, sizeof(p->interface), "%.*s", IF_NAMESIZE - 1,
		         it->value);
	else
		return pr_fail(p, it->line, "invalid interface: %s", it->value);

	if ((it = seen[KEY_SSID])) {
		if (it->len == 0 || it->len > SSID_MAX)
			return pr_fail(p, it->line, "ssid must have 1 to %d bytes",
			               SSID_MAX);
		memcpy(p->ssid, it->value, it->len);
		p->ssid_len = it->len;
		p->has_ssid = 1;
	}

	it = seen[KEY_SECURITY];
	if (p->has_ssid && !it)
		return pr_fail(p, 0, "missing security");
	if (!p->has_ssid && it)
		return pr_fail(p, it->line, "security requires ssid");
	if (it) {
		if (strcmp(it->value, "wpa3") == 0)
			p->security = SEC_WPA3;
		else if (strcmp(it->value, "wpa2") == 0)
			p->security = SEC_WPA2;
		else if (strcmp(it->value, "open") == 0)
			p->security = SEC_OPEN;
		else
			return pr_fail(p, it->line, "invalid security: %s", it->value);
	}

	it = seen[KEY_PASSPHRASE];
	int needs = p->security == SEC_WPA2 || p->security == SEC_WPA3;
	if (needs && !it)
		return pr_fail(p, 0, "missing passphrase");
	if (!needs && it)
		return pr_fail(p, it->line,
		               "passphrase requires security wpa2 or wpa3");
	if (it && parse_passphrase(p, it) < 0)
		return -1;

	if (parse_address(p, seen[KEY_ADDRESS]) < 0)
		return -1;
	if (seen[KEY_GATEWAY] && parse_gateway(p, seen[KEY_GATEWAY]) < 0)
		return -1;
	if (parse_dns(p, seen[KEY_DNS]) < 0)
		return -1;
	if (parse_bool(p, seen[KEY_AUTOCONNECT], &p->autoconnect) < 0 ||
	    parse_bool(p, seen[KEY_TRUSTED], &p->trusted) < 0)
		return -1;
	return 0;
}

int profile_parse(struct profile *p, const char *name, const char *text,
                  size_t len, int exposed) {
	static struct grammar g;
	const struct gr_item *seen[KEY_COUNT] = {0};

	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);

	if (gr_parse(text, len, &g) < 0)
		return pr_fail(p, g.err_line, "%s", g.err);
	if (check_structure(p, &g, seen) < 0 || check_values(p, seen) < 0)
		return -1;
	if (exposed && p->has_passphrase)
		return pr_fail(p, 0,
		               "credentials in a file readable by group or others");
	p->valid = 1;
	return 0;
}
