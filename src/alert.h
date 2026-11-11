#ifndef TUNDRA_ALERT_H
#define TUNDRA_ALERT_H

#include <tundra/blueice.h>

enum alert_source { SRC_DHCPC, SRC_SENSOR };

int alert_allowed(enum alert_source src, const char *by);

#endif
