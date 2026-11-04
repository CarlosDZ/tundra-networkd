#include "alert.c"
#include "test.h"

static struct alert a;

static int ok_line(const char *line) { return alert_parse(line, &a) == 0; }

int main(void) {
	CHECK(ok_line("alert 111/8 stop by tundra-blueice-arp on enp8s0 ts "
	              "1762171512.347 reply force,disconnect info case D gw "
	              "10.77.0.1 known 52:54:00:ac:c9:ff seen "
	              "52:54:00:ac:c9:ff,02:bb:bb:bb:bb:bb forged 0 -- gateway "
	              "impersonation") &&
	          a.level == 8 && a.action == AL_STOP &&
	          strcmp(a.by, "tundra-blueice-arp") == 0 &&
	          strcmp(a.on, "enp8s0") == 0 &&
	          strcmp(a.reply, "force,disconnect") == 0 &&
	          strcmp(a.kase, "D") == 0,
	      "the example of ADR-025");
	CHECK(ok_line("alert 111/10 warn by tundra-blueice-arp on enp8s0 ts 1.000 "
	              "reply ack,disconnect info case mac-net") &&
	          a.level == 10 && a.action == AL_WARN,
	      "level 10, a warning, no text");
	CHECK(ok_line("alert 111/2 reinforce by x on y ts 1 reply ack info case z "
	              "extra 1") &&
	          a.action == AL_REINFORCE,
	      "reinforce, unknown keys ignored");
	CHECK(!ok_line("alert 111/0 stop by x on y reply r info case c"),
	      "level 0");
	CHECK(!ok_line("alert 111/11 stop by x on y reply r info case c"),
	      "level 11");
	CHECK(!ok_line("alert 111/08 stop by x on y reply r info case c"),
	      "a level with a leading zero");
	CHECK(!ok_line("alert 2/8 stop by x on y reply r info case c"),
	      "not a 111");
	CHECK(!ok_line("alert 111/8 halt by x on y reply r info case c"),
	      "an unknown action");
	CHECK(!ok_line("alert 111/8 stop on y reply r info case c"), "without by");
	CHECK(!ok_line("alert 111/8 stop by x on y reply r"), "without case");
	CHECK(!ok_line("alert 111/8 stop by x on y reply r case c"),
	      "case outside info");
	CHECK(!ok_line("alert 111/8 stop by x by z on y reply r info case c"),
	      "a repeated by");
	CHECK(!ok_line("alert 111/8 stop by x on -- y reply r info case c"),
	      "a key without value before the text");
	CHECK(!ok_line("dhcpc: bound to 10.0.0.1"), "an ordinary line");
	CHECK(!ok_line("alerting 111/8 stop by x on y reply r info case c"),
	      "a word that starts like alert");

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
