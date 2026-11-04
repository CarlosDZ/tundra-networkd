#ifndef TUNDRA_SPAWN_H
#define TUNDRA_SPAWN_H

#include <sys/types.h>

pid_t spawn_child(const char *path, char *const argv[], int *out);

#endif
