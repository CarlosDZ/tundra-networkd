#ifndef TUNDRA_LINK_H
#define TUNDRA_LINK_H

#include <net/if.h>

#define LINK_EVENTS_MAX 64

struct link_ev {
	int gone;
	int ifindex;
	unsigned short arptype;
	int up;
	char name[IF_NAMESIZE];
};

int link_open(void);
int link_dump(int fd);
int link_read(int fd, struct link_ev *evs, int max);

#endif
