#include "resolv.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int resolv_source(const char *ifname, const struct profile *p,
                  const struct iface_conf *conf, const struct lease_info *li,
                  struct resolv_src *out) {
	if (!p || !li)
		return -1;
	memset(out, 0, sizeof(*out));
	snprintf(out->ifname, sizeof(out->ifname), "%s", ifname);
	out->metric = conf && conf->has_metric ? conf->metric : 0;
	if (p->dns == DNS_LEASE) {
		out->n = li->ndns;
		memcpy(out->ns, li->dns, sizeof(out->ns));
	} else if (p->dns == DNS_LIST) {
		out->n = p->ndns;
		memcpy(out->ns, p->dns_list, sizeof(out->ns));
	}
	return 0;
}

static int by_metric(const void *a, const void *b) {
	const struct resolv_src *x = a, *y = b;
	if (x->metric != y->metric)
		return x->metric < y->metric ? -1 : 1;
	return strcmp(x->ifname, y->ifname);
}

int resolv_build(struct resolv_src *src, int n, char *out, size_t size) {
	uint32_t picked[DNS_MAX];
	int npicked = 0;
	size_t used = 0;
	if (n > 0)
		qsort(src, (size_t)n, sizeof(*src), by_metric);
	out[0] = '\0';
	for (int i = 0; i < n && npicked < DNS_MAX; i++) {
		for (int j = 0; j < src[i].n && npicked < DNS_MAX; j++) {
			int seen = 0;
			for (int k = 0; k < npicked; k++)
				seen |= picked[k] == src[i].ns[j];
			if (seen)
				continue;
			char a[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &src[i].ns[j], a, sizeof(a));
			int w = snprintf(out + used, size - used, "nameserver %s\n", a);
			if (w < 0 || (size_t)w >= size - used)
				return -1;
			used += (size_t)w;
			picked[npicked++] = src[i].ns[j];
		}
	}
	return npicked;
}
