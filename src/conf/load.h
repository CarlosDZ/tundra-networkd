#ifndef TUNDRA_LOAD_H
#define TUNDRA_LOAD_H

#include "ifaces.h"
#include "profile.h"

#define PROFILES_MAX 64
#define CONF_FILE_MAX 16384

struct config {
	struct ifaces ifaces;
	struct profile profiles[PROFILES_MAX];
	int n;
	int truncated;
};

int conf_load(const char *dir, struct config *c);

#endif
