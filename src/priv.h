#ifndef TUNDRA_PRIV_H
#define TUNDRA_PRIV_H

#include <stdint.h>

struct priv_caps {
	uint64_t effective;
	uint64_t permitted;
	uint64_t inheritable;
};

/* The sets the daemon keeps: nothing it can use, and as inheritable only the
 * capabilities its children activate through their file capabilities
 * (ADR-031). */
struct priv_caps priv_shed_sets(struct priv_caps now);

/* Applies priv_shed_sets to the process unless it runs as root, whose
 * children get their capabilities anyway. */
int priv_shed(void);

#endif
