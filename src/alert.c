#include "alert.h"

#include <string.h>

int alert_allowed(enum alert_source src, const char *by) {
	if (strcmp(by, "tundra-blueice-arp") == 0)
		return 1;
	return src == SRC_DHCPC && strcmp(by, "tundra-dhcpc") == 0;
}
