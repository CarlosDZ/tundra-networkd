#ifndef TUNDRA_LEASEINFO_H
#define TUNDRA_LEASEINFO_H

#include "profile.h"

#include <stddef.h>
#include <stdint.h>

#define LEASEINFO_ADDR_MAX 19

struct lease_info {
	uint32_t address;
	int prefix;
	uint32_t router;
	uint32_t dns[DNS_MAX];
	int ndns;
};

int leaseinfo_parse(const char *text, size_t len, struct lease_info *out);
void leaseinfo_address(const struct lease_info *li,
                       char out[LEASEINFO_ADDR_MAX]);

#endif
