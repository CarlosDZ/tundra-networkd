#ifndef TUNDRA_RESOLV_H
#define TUNDRA_RESOLV_H

#include "ifaces.h"
#include "leaseinfo.h"
#include "profile.h"

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>

#define RESOLV_TEXT_MAX 128

struct resolv_src {
	char ifname[IF_NAMESIZE];
	unsigned int metric;
	uint32_t ns[DNS_MAX];
	int n;
};

int resolv_source(const char *ifname, const struct profile *p,
                  const struct iface_conf *conf, const struct lease_info *li,
                  struct resolv_src *out);
int resolv_build(struct resolv_src *src, int n, char *out, size_t size);

#endif
