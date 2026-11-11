#include "alert.c"
#include "alerts.c"
#include "test.h"

static struct alert_store st;

static const char *line_d =
    "alert 111/8 stop by tundra-blueice-arp on tnd0 ts 1.000 reply "
    "force,disconnect info case D gw 10.79.0.1 -- gateway impersonation";
static const char *line_w =
    "alert 111/2 warn by tundra-blueice-arp on tnd1 ts 2.000 reply "
    "ack,disconnect info case mac-net mac 01:00:5e:00:00:01";

static unsigned int add(const char *profile, const char *line) {
	struct blueice_parsed a;
	if (blueice_alert_parse(line, &a) < 0)
		return 0;
	return alerts_add(&st, profile, &a, line);
}

int main(void) {
	char out[1024];
	alerts_init(&st);
	unsigned int d = add("casa", line_d), w = add(NULL, line_w);
	CHECK(d == 1 && w == 2 && st.n == 2, "ids start at 1 and grow");
	CHECK(alerts_format(alerts_find(&st, 1), out, sizeof(out)) >= 0 &&
	          strncmp(out, "ALERT 1 casa alert 111/8 stop ", 30) == 0,
	      "the forwarded line carries id and profile");
	CHECK(alerts_format(alerts_find(&st, 2), out, sizeof(out)) >= 0 &&
	          strncmp(out, "ALERT 2 - alert 111/2 warn ", 27) == 0,
	      "an alert without connection has profile -");
	CHECK(alerts_offers(alerts_find(&st, 1), "force") &&
	          alerts_offers(alerts_find(&st, 1), "disconnect") &&
	          !alerts_offers(alerts_find(&st, 1), "ack") &&
	          !alerts_offers(alerts_find(&st, 1), "forc"),
	      "the options come from reply");
	CHECK(alerts_ids_on(&st, "tnd0", out, sizeof(out)) == 0 &&
	          strcmp(out, "1") == 0,
	      "the alerts of one interface");
	CHECK(alerts_ids_on(&st, "tnd9", out, sizeof(out)) == 0 &&
	          strcmp(out, "-") == 0,
	      "an interface without alerts");

	char file[2048] = "";
	for (int i = 0; i < st.n; i++) {
		alerts_format(&st.items[i], out, sizeof(out));
		strcat(file, out);
		strcat(file, "\n");
	}
	struct alert_store back;
	alerts_init(&back);
	strcat(file, "ALERT x casa alert broken\nALERT 9 casa not an alert\n");
	CHECK(alerts_load(&back, file, strlen(file)) == 2 && back.n == 2 &&
	          back.next_id == 3 && alerts_find(&back, 1)->a.level == 8 &&
	          strcmp(alerts_find(&back, 1)->profile, "casa") == 0,
	      "the file loads back, skipping broken lines");
	alerts_remove(&back, 1);
	CHECK(back.n == 1 && !alerts_find(&back, 1) && alerts_find(&back, 2),
	      "an answered alert is removed");

	alerts_init(&st);
	add("casa", line_d);
	for (int i = 0; i < ALERTS_MAX; i++)
		add("casa", line_w);
	CHECK(st.n == ALERTS_MAX && alerts_find(&st, 1) && !alerts_find(&st, 2),
	      "when full, the oldest warning goes first, not the stop");
	return test_summary();
}
