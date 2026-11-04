#ifndef TUNDRA_LEASE_H
#define TUNDRA_LEASE_H

#include <limits.h>

#define LEASE_EVENTS_MAX 32

struct lease_ev {
	int removed;
	char file[NAME_MAX + 1];
};

int lease_watch_open(const char *dir);
int lease_watch_read(int fd, struct lease_ev *evs, int max);

#endif
