#include "hwaddr.h"

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

static int request(const char *ifname, unsigned long req, struct ifreq *ifr) {
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	snprintf(ifr->ifr_name, sizeof(ifr->ifr_name), "%s", ifname);
	int r = ioctl(fd, req, ifr);
	close(fd);
	return r;
}

int hwaddr_current(const char *ifname, unsigned char out[HWADDR_LEN]) {
	struct ifreq ifr = {0};
	if (request(ifname, SIOCGIFHWADDR, &ifr) < 0)
		return -1;
	memcpy(out, ifr.ifr_hwaddr.sa_data, HWADDR_LEN);
	return 0;
}

/* The address of the hardware, whatever the interface uses now. A virtual
 * interface has none: -1. */
int hwaddr_permanent(const char *ifname, unsigned char out[HWADDR_LEN]) {
	union {
		struct ethtool_perm_addr hdr;
		unsigned char room[sizeof(struct ethtool_perm_addr) + HWADDR_LEN];
	} perm = {.hdr = {.cmd = ETHTOOL_GPERMADDR, .size = HWADDR_LEN}};
	struct ifreq ifr = {0};
	ifr.ifr_data = (void *)&perm;
	if (request(ifname, SIOCETHTOOL, &ifr) < 0 || perm.hdr.size != HWADDR_LEN)
		return -1;
	const unsigned char *addr = perm.room + sizeof(struct ethtool_perm_addr);
	int any = 0;
	for (int i = 0; i < HWADDR_LEN; i++)
		any |= addr[i];
	if (!any)
		return -1;
	memcpy(out, addr, HWADDR_LEN);
	return 0;
}

/* Unicast and locally administered, so it never collides with a vendor's. */
int hwaddr_random(unsigned char out[HWADDR_LEN]) {
	if (getrandom(out, HWADDR_LEN, 0) != HWADDR_LEN)
		return -1;
	out[0] = (unsigned char)((out[0] & 0xfc) | 0x02);
	return 0;
}

int hwaddr_is_up(const char *ifname) {
	struct ifreq ifr = {0};
	if (request(ifname, SIOCGIFFLAGS, &ifr) < 0)
		return -1;
	return (ifr.ifr_flags & IFF_UP) != 0;
}

void hwaddr_format(const unsigned char mac[HWADDR_LEN], char out[HWADDR_TEXT]) {
	snprintf(out, HWADDR_TEXT, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
	         mac[2], mac[3], mac[4], mac[5]);
}
