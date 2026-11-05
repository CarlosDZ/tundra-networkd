#ifndef TUNDRA_IFACES_H
#define TUNDRA_IFACES_H

#include "profile.h"

#include <net/if.h>
#include <stddef.h>

#define IFACES_MAX 32
#define TABLE_MAX 64

struct iface_conf {
	char name[IF_NAMESIZE];
	int has_metric;
	unsigned int metric;
	int has_table;
	char table[TABLE_MAX + 1];
	int has_grace;
	unsigned int grace_ms;
	int has_wifi;
	int wifi;
	char wired_error[CONF_ERR_MAX];
	int valid;
	char error[CONF_ERR_MAX];
};

struct ifaces {
	struct iface_conf items[IFACES_MAX];
	int n;
	int valid;
	char error[CONF_ERR_MAX];
};

int ifaces_parse(struct ifaces *f, const char *text, size_t len);
const struct iface_conf *ifaces_find(const struct ifaces *f, const char *name);

#endif
