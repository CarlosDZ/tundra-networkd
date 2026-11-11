#include "alert.c"
#include "test.h"

int main(void) {
	CHECK(alert_allowed(SRC_DHCPC, "tundra-dhcpc") &&
	          alert_allowed(SRC_DHCPC, "tundra-blueice-arp"),
	      "the dhcpc pipe carries dhcpc and its probe");
	CHECK(alert_allowed(SRC_SENSOR, "tundra-blueice-arp") &&
	          !alert_allowed(SRC_SENSOR, "tundra-dhcpc"),
	      "the sensor pipe only carries the sensor");
	CHECK(!alert_allowed(SRC_DHCPC, "tundra-networkd"),
	      "nobody signs as tundra-networkd");
	return test_summary();
}
