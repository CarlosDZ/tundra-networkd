#include "profile.h"
#include <tundra/conf.h>

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum section { S_MATCH, S_APPLY };
enum kind { K_TOKEN, K_TEXT };

static const struct {
	const char *name;
	enum section sec;
	enum kind kind;
} keys[PKEY_COUNT] = {
    [PKEY_INTERFACE] = {"interface", S_MATCH, K_TOKEN},
    [PKEY_SSID] = {"ssid", S_MATCH, K_TEXT},
    [PKEY_SECURITY] = {"security", S_MATCH, K_TOKEN},
    [PKEY_PASSPHRASE] = {"passphrase", S_APPLY, K_TEXT},
    [PKEY_ADDRESS] = {"address", S_APPLY, K_TOKEN},
    [PKEY_GATEWAY] = {"gateway", S_APPLY, K_TOKEN},
    [PKEY_DNS] = {"dns", S_APPLY, K_TOKEN},
    [PKEY_AUTOCONNECT] = {"autoconnect", S_APPLY, K_TOKEN},
    [PKEY_TRUSTED] = {"trusted", S_APPLY, K_TOKEN},
    [PKEY_MAC] = {"mac", S_APPLY, K_TOKEN},
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

static int hexdigit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* keep, random or a unicast address that is not all zeros. */
int conf_parse_mac(const char *s, struct mac_policy *out) {
	memset(out, 0, sizeof(*out));
	if (strcmp(s, "keep") == 0) {
		out->mode = MAC_KEEP;
		return 0;
	}
	if (strcmp(s, "random") == 0) {
		out->mode = MAC_RANDOM;
		return 0;
	}
	if (strlen(s) != 3 * MAC_LEN - 1)
		return -1;
	int any = 0;
	for (int i = 0; i < MAC_LEN; i++) {
		int hi = hexdigit(s[3 * i]), lo = hexdigit(s[3 * i + 1]);
		if (hi < 0 || lo < 0 || (i < MAC_LEN - 1 && s[3 * i + 2] != ':'))
			return -1;
		out->addr[i] = (unsigned char)(hi << 4 | lo);
		any |= out->addr[i];
	}
	if (!any || (out->addr[0] & 1))
		return -1;
	out->mode = MAC_FIXED;
	return 0;
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

static int valid_name(const char *s, size_t n) {
	if (n == 0 || n > PROFILE_NAME_MAX || s[0] == '.')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

int profile_valid_name(const char *name) {
	return valid_name(name, strlen(name));
}

int profile_name_from_file(const char *file, char name[PROFILE_NAME_MAX + 1]) {
	size_t n = strlen(file);
	if (n <= 4 || strcmp(file + n - 4, ".tnc") != 0)
		return -1;
	n -= 4;
	if (!valid_name(file, n))
		return -2;
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

static int parse_address(struct profile *p, const struct tconf_item *it) {
	if (strcmp(it->value, "dhcp") == 0) {
		p->address = ADDR_DHCP;
		return 0;
	}
	char ip[TCONF_VALUE_MAX];
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

static int parse_gateway(struct profile *p, const struct tconf_item *it) {
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

static int parse_dns(struct profile *p, const struct tconf_item *it) {
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

	char buf[TCONF_VALUE_MAX];
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

static int parse_bool(struct profile *p, const struct tconf_item *it,
                      int *out) {
	if (strcmp(it->value, "yes") == 0)
		*out = 1;
	else if (strcmp(it->value, "no") == 0)
		*out = 0;
	else
		return pr_fail(p, it->line, "%s must be yes or no", it->key);
	return 0;
}

static int parse_passphrase(struct profile *p, const struct tconf_item *it) {
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

int profile_key(const char *name) {
	for (int k = 0; k < PKEY_COUNT; k++)
		if (strcmp(keys[k].name, name) == 0)
			return k;
	return -1;
}

static int check_structure(struct profile *p, const struct tconf *g,
                           const struct tconf_item *seen[PKEY_COUNT]) {
	int block_sec[TCONF_BLOCKS_MAX];
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
		const struct tconf_item *it = &g->items[i];
		if (it->block < 0)
			return pr_fail(p, it->line, "%s outside a section", it->key);
		int k = profile_key(it->key);
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

static const char *missing(const struct tconf_item *seen[PKEY_COUNT]) {
	static const int required[] = {PKEY_INTERFACE, PKEY_ADDRESS, PKEY_DNS,
	                               PKEY_AUTOCONNECT, PKEY_TRUSTED};
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
		if (!seen[required[i]])
			return keys[required[i]].name;
	return NULL;
}

static int check_values(struct profile *p,
                        const struct tconf_item *seen[PKEY_COUNT]) {
	const struct tconf_item *it;

	const char *absent = missing(seen);
	if (absent)
		return pr_fail(p, 0, "missing %s", absent);

	it = seen[PKEY_INTERFACE];
	if (strcmp(it->value, "*") == 0)
		p->any_interface = 1;
	else if (conf_valid_ifname(it->value))
		snprintf(p->interface, sizeof(p->interface), "%.*s", IF_NAMESIZE - 1,
		         it->value);
	else
		return pr_fail(p, it->line, "invalid interface: %s", it->value);

	if ((it = seen[PKEY_SSID])) {
		if (it->len == 0 || it->len > SSID_MAX)
			return pr_fail(p, it->line, "ssid must have 1 to %d bytes",
			               SSID_MAX);
		memcpy(p->ssid, it->value, it->len);
		p->ssid_len = it->len;
		p->has_ssid = 1;
	}

	it = seen[PKEY_SECURITY];
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

	it = seen[PKEY_PASSPHRASE];
	int needs = p->security == SEC_WPA2 || p->security == SEC_WPA3;
	if (needs && !it)
		return pr_fail(p, 0, "missing passphrase");
	if (!needs && it)
		return pr_fail(p, it->line,
		               "passphrase requires security wpa2 or wpa3");
	if (it && parse_passphrase(p, it) < 0)
		return -1;

	if (parse_address(p, seen[PKEY_ADDRESS]) < 0)
		return -1;
	if (seen[PKEY_GATEWAY] && parse_gateway(p, seen[PKEY_GATEWAY]) < 0)
		return -1;
	if (parse_dns(p, seen[PKEY_DNS]) < 0)
		return -1;
	if (parse_bool(p, seen[PKEY_AUTOCONNECT], &p->autoconnect) < 0 ||
	    parse_bool(p, seen[PKEY_TRUSTED], &p->trusted) < 0)
		return -1;
	if ((it = seen[PKEY_MAC]) && conf_parse_mac(it->value, &p->mac) < 0)
		return pr_fail(p, it->line, "invalid mac: %s", it->value);
	return 0;
}

int profile_parse(struct profile *p, const char *name, const char *text,
                  size_t len, int exposed) {
	static struct tconf g;
	const struct tconf_item *seen[PKEY_COUNT] = {0};

	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);

	if (tconf_parse(text, len, &g) < 0)
		return pr_fail(p, g.err_line, "%s", g.err);
	if (check_structure(p, &g, seen) < 0 || check_values(p, seen) < 0)
		return -1;
	if (exposed && p->has_passphrase)
		return pr_fail(
		    p, 0,
		    "credentials in a file readable by another group or by others");
	p->valid = 1;
	return 0;
}

/* A request without passphrase for a network that needs one keeps the stored
 * one: no client can read it to send it back. */
void profile_draft_keep(struct profile_draft *d, const struct profile *old) {
	struct draft_key *pass = &d->key[PKEY_PASSPHRASE];
	const char *sec = d->key[PKEY_SECURITY].value;
	const struct draft_key *ssid = &d->key[PKEY_SSID];
	if (pass->named || !old || !old->valid || !old->has_passphrase)
		return;
	/* A passphrase belongs to its network: it is not carried to another. */
	if (ssid->len != old->ssid_len ||
	    memcmp(ssid->value, old->ssid, ssid->len) != 0)
		return;
	if (strcmp(sec, "wpa2") != 0 && strcmp(sec, "wpa3") != 0)
		return;
	pass->len = strlen(old->passphrase);
	memcpy(pass->value, old->passphrase, pass->len + 1);
	pass->named = 1;
}

struct text {
	char *buf;
	size_t size, len;
	int full;
};

static void text_add(struct text *t, const char *s, size_t n) {
	if (t->full || n >= t->size - t->len) {
		t->full = 1;
		return;
	}
	memcpy(t->buf + t->len, s, n);
	t->len += n;
	t->buf[t->len] = '\0';
}

static void text_str(struct text *t, const char *s) {
	text_add(t, s, strlen(s));
}

/* Bytes of 0x80 and above stay as they are: a passphrase of 128 bytes with
 * every one escaped would not fit the line limit of the grammar. */
static void text_quoted(struct text *t, const struct draft_key *k) {
	static const char hex[] = "0123456789abcdef";
	text_str(t, "\"");
	for (size_t i = 0; i < k->len; i++) {
		unsigned char c = (unsigned char)k->value[i];
		if (c == '"' || c == '\\') {
			char esc[2] = {'\\', (char)c};
			text_add(t, esc, 2);
		} else if (c < 0x20 || c == 0x7f) {
			char esc[4] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
			text_add(t, esc, 4);
		} else {
			text_add(t, k->value + i, 1);
		}
	}
	text_str(t, "\"");
}

/* A bare value ends at a '#' or at the line and cannot hold a quote: any of
 * them would turn one value into other lines of the file. */
static int bare_ok(const struct draft_key *k) {
	for (size_t i = 0; i < k->len; i++) {
		unsigned char c = (unsigned char)k->value[i];
		if (c <= 0x20 || c > 0x7e || c == '#' || c == '"')
			return 0;
	}
	return 1;
}

static int text_section(struct profile *p, struct text *t,
                        const struct profile_draft *d, enum section sec) {
	text_str(t, section_name(sec));
	text_str(t, " {\n");
	for (int k = 0; k < PKEY_COUNT; k++) {
		const struct draft_key *v = &d->key[k];
		if (keys[k].sec != sec || !v->len)
			continue;
		if (keys[k].kind == K_TOKEN && !bare_ok(v))
			return pr_fail(p, 0, "invalid value for %s", keys[k].name);
		text_str(t, "\t");
		text_str(t, keys[k].name);
		text_str(t, " = ");
		if (keys[k].kind == K_TEXT)
			text_quoted(t, v);
		else
			text_add(t, v->value, v->len);
		text_str(t, "\n");
	}
	text_str(t, "}\n");
	return 0;
}

/* Writes the .tnc of a draft and checks it as a loaded file is checked. The
 * length of the text, or -1 with the reason in p->error. */
int profile_build(struct profile *p, const char *name,
                  const struct profile_draft *d, char *text, size_t size) {
	struct text t = {.buf = text, .size = size};
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	if (size == 0)
		return pr_fail(p, 0, "profile too long");
	text[0] = '\0';
	if (text_section(p, &t, d, S_MATCH) < 0)
		return -1;
	text_str(&t, "\n");
	if (text_section(p, &t, d, S_APPLY) < 0)
		return -1;
	if (t.full)
		return pr_fail(p, 0, "profile too long");
	if (profile_parse(p, name, text, t.len, 0) < 0) {
		/* The line is one of a text the client never saw. */
		int skip = 0;
		if (sscanf(p->error, "line %*d: %n", &skip) == 0 && skip > 0)
			memmove(p->error, p->error + skip, strlen(p->error + skip) + 1);
		return -1;
	}
	return (int)t.len;
}
