#include "leaseinfo.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int parse_ip(const char *s, size_t len, uint32_t *out) {
	char buf[INET_ADDRSTRLEN];
	struct in_addr a;
	if (len == 0 || len >= sizeof(buf))
		return -1;
	memcpy(buf, s, len);
	buf[len] = '\0';
	if (inet_pton(AF_INET, buf, &a) != 1)
		return -1;
	*out = a.s_addr;
	return 0;
}

static int mask_prefix(uint32_t mask) {
	uint32_t m = ntohl(mask);
	int n = 0;
	while (n < 32 && (m & (0x80000000u >> n)))
		n++;
	return m == (n ? 0xffffffffu << (32 - n) : 0) ? n : -1;
}

static void parse_dns(const char *s, const char *end, struct lease_info *out) {
	int n = 0;
	while (s < end && n < DNS_MAX) {
		const char *comma = memchr(s, ',', (size_t)(end - s));
		const char *tend = comma ? comma : end;
		if (parse_ip(s, (size_t)(tend - s), &out->dns[n]) < 0) {
			n = 0;
			break;
		}
		n++;
		s = tend + 1;
	}
	out->ndns = n;
}

int leaseinfo_parse(const char *text, size_t len, struct lease_info *out) {
	const char *end = text + len;
	uint32_t mask = 0;
	int has_addr = 0, has_mask = 0;
	memset(out, 0, sizeof(*out));
	for (const char *line = text; line < end;) {
		const char *nl = memchr(line, '\n', (size_t)(end - line));
		const char *stop = nl ? nl : end;
		const char *eq = memchr(line, '=', (size_t)(stop - line));
		if (eq) {
			size_t klen = (size_t)(eq - line);
			const char *v = eq + 1;
			size_t vlen = (size_t)(stop - v);
			if (klen == 7 && strncmp(line, "address", 7) == 0)
				has_addr = parse_ip(v, vlen, &out->address) == 0;
			else if (klen == 4 && strncmp(line, "mask", 4) == 0)
				has_mask = parse_ip(v, vlen, &mask) == 0;
			else if (klen == 3 && strncmp(line, "dns", 3) == 0)
				parse_dns(v, stop, out);
		}
		line = stop + 1;
	}
	out->prefix = has_mask ? mask_prefix(mask) : -1;
	return has_addr && out->prefix >= 0 ? 0 : -1;
}

void leaseinfo_address(const struct lease_info *li,
                       char out[LEASEINFO_ADDR_MAX]) {
	char a[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &li->address, a, sizeof(a));
	snprintf(out, LEASEINFO_ADDR_MAX, "%s/%d", a, li->prefix);
}
