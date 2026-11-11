#include "ifaces.h"
#include "grammar.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IFACE_KEYS 5

static void set_error(char *dst, int line, const char *fmt, va_list ap) {
	char msg[CONF_ERR_MAX - 16];
	vsnprintf(msg, sizeof(msg), fmt, ap);
	if (line > 0)
		snprintf(dst, CONF_ERR_MAX, "line %d: %.100s", line, msg);
	else
		snprintf(dst, CONF_ERR_MAX, "%s", msg);
}

static int file_fail(struct ifaces *f, int line, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	set_error(f->error, line, fmt, ap);
	va_end(ap);
	f->valid = 0;
	return -1;
}

static void iface_fail(struct iface_conf *c, int line, const char *fmt, ...) {
	va_list ap;
	if (!c->valid)
		return;
	va_start(ap, fmt);
	set_error(c->error, line, fmt, ap);
	va_end(ap);
	c->valid = 0;
}

static int parse_uint(const char *s, unsigned int *out) {
	if (s[0] < '0' || s[0] > '9')
		return -1;
	char *end;
	errno = 0;
	unsigned long v = strtoul(s, &end, 10);
	if (errno || *end != '\0' || v > 0xffffffffUL)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

static int valid_table(const char *s) {
	size_t n = strlen(s);
	if (n == 0 || n > TABLE_MAX || s[0] == '.')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return strcmp(s, "local") != 0 && strcmp(s, "default") != 0 &&
	       strcmp(s, "0") != 0 && strcmp(s, "253") != 0 &&
	       strcmp(s, "255") != 0;
}

static void apply_item(struct iface_conf *c, const struct gr_item *it,
                       int *seen) {
	static const char *const names[] = {"metric", "table", "carrier-grace",
	                                    "wifi", "mac"};
	int k = -1;
	for (int i = 0; i < IFACE_KEYS; i++)
		if (strcmp(it->key, names[i]) == 0)
			k = i;

	if (k < 0) {
		iface_fail(c, it->line, "unknown key %s", it->key);
		return;
	}
	if (seen[k]++) {
		iface_fail(c, it->line, "repeated key %s", it->key);
		return;
	}
	if (it->quoted) {
		iface_fail(c, it->line, "%s must not be quoted", it->key);
		return;
	}

	switch (k) {
	case 0:
		if (parse_uint(it->value, &c->metric) < 0)
			iface_fail(c, it->line, "invalid metric: %s", it->value);
		c->has_metric = 1;
		break;
	case 1:
		if (!valid_table(it->value))
			iface_fail(c, it->line, "invalid table: %s", it->value);
		snprintf(c->table, sizeof(c->table), "%.*s", TABLE_MAX, it->value);
		c->has_table = 1;
		break;
	case 2:
		if (parse_uint(it->value, &c->grace_ms) < 0)
			iface_fail(c, it->line, "invalid carrier-grace: %s", it->value);
		c->has_grace = 1;
		break;
	case 3:
		if (strcmp(it->value, "yes") != 0 && strcmp(it->value, "no") != 0)
			iface_fail(c, it->line, "invalid wifi: %s", it->value);
		c->wifi = strcmp(it->value, "no") != 0;
		c->has_wifi = 1;
		snprintf(c->wired_error, sizeof(c->wired_error),
		         "line %d: wifi on a wired interface", it->line);
		break;
	case 4:
		if (conf_parse_mac(it->value, &c->mac) < 0)
			iface_fail(c, it->line, "invalid mac: %s", it->value);
		break;
	}
}

int ifaces_parse(struct ifaces *f, const char *text, size_t len) {
	static struct grammar g;
	int slot[GR_BLOCKS_MAX];
	int seen[IFACES_MAX][IFACE_KEYS];

	memset(f, 0, sizeof(*f));
	memset(seen, 0, sizeof(seen));
	f->valid = 1;
	if (gr_parse(text, len, &g) < 0)
		return file_fail(f, g.err_line, "%s", g.err);

	for (int b = 0; b < g.nblocks; b++) {
		const struct gr_block *blk = &g.blocks[b];
		int dup = -1;
		for (int i = 0; i < f->n; i++)
			if (strcmp(f->items[i].name, blk->name) == 0)
				dup = i;
		if (dup >= 0) {
			iface_fail(&f->items[dup], blk->line, "repeated block %s",
			           blk->name);
			slot[b] = dup;
			continue;
		}
		if (f->n == IFACES_MAX)
			return file_fail(f, blk->line, "more than %d interfaces",
			                 IFACES_MAX);
		struct iface_conf *c = &f->items[f->n];
		c->valid = 1;
		snprintf(c->name, sizeof(c->name), "%.*s", IF_NAMESIZE - 1, blk->name);
		if (!conf_valid_ifname(blk->name))
			iface_fail(c, blk->line, "invalid interface: %s", blk->name);
		slot[b] = f->n++;
	}

	for (int i = 0; i < g.n; i++) {
		const struct gr_item *it = &g.items[i];
		if (it->block < 0)
			return file_fail(f, it->line, "%s outside a block", it->key);
		int s = slot[it->block];
		apply_item(&f->items[s], it, seen[s]);
	}
	return 0;
}

const struct iface_conf *ifaces_find(const struct ifaces *f, const char *name) {
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->items[i].name, name) == 0)
			return &f->items[i];
	return NULL;
}
