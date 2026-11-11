#ifndef TUNDRA_STATIC_H
#define TUNDRA_STATIC_H

#include "core.h"

int static_apply(const struct action *a);
int static_remove(const struct action *a);
int static_set_mac(const struct action *a);
int static_link_up(const struct action *a);
int static_link_down(const struct action *a);

#endif
