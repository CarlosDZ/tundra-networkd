#include "conf/grammar.c"
#include "conf/ifaces.c"
#include "conf/profile.c"
#include "core.c"
#include "test.h"

#define EXITED(n) ((n) << 8)

static struct config cfg;
static struct core c;
static struct actions out;

static void reset(const char *ifaces) {
	memset(&cfg, 0, sizeof(cfg));
	ifaces_parse(&cfg.ifaces, ifaces, strlen(ifaces));
	core_init(&c, &cfg);
	out.n = 0;
}

static void add(const char *name, const char *iface, const char *apply) {
	char buf[512];
	snprintf(buf, sizeof(buf), "match {\n interface = %s\n}\napply {\n%s}\n",
	         iface, apply);
	profile_parse(&cfg.profiles[cfg.n++], name, buf, strlen(buf), 0);
}

#define AUTO                                                                   \
	" address = dhcp\n dns = lease\n autoconnect = yes\n trusted = no\n"
#define MANUAL                                                                 \
	" address = dhcp\n dns = lease\n autoconnect = no\n trusted = no\n"
#define STATIC                                                                 \
	" address = 192.168.1.50/24\n dns = none\n autoconnect = yes\n"            \
	" trusted = no\n"

static const struct action *find_act(enum act_type t) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == t)
			return &out.a[i];
	return NULL;
}

static int count(enum act_type t) {
	int n = 0;
	for (int i = 0; i < out.n; i++)
		n += out.a[i].type == t;
	return n;
}

static int logged(const char *text) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == A_LOG && strstr(out.a[i].msg, text))
			return 1;
	return 0;
}

static enum conn_state st(const char *ifname) {
	return core_find(&c, ifname)->st;
}

static void test_autoconnect(void) {
	reset("tnd0 {\n metric = 100\n table = casa\n carrier-grace = 3000\n}\n");
	add("10-casa", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	const struct action *a = find_act(A_SPAWN_DHCPC);
	CHECK(a && strcmp(a->iface, "tnd0") == 0 &&
	          strcmp(a->profile, "10-casa") == 0,
	      "an autoconnect profile launches tundra-dhcpc");
	CHECK(a && a->has_metric && a->metric == 100 && a->has_table &&
	          strcmp(a->table, "casa") == 0 && a->grace_ms == 3000,
	      "metric, table and carrier-grace come from interfaces");
	out.n = 0;
	core_link(&c, "tnd0", 1, 5, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "a second link event does not relaunch");

	reset("");
	add("10-manual", "tnd0", MANUAL);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "autoconnect = no waits for the user");
	CHECK(core_connect(&c, "10-manual", "tnd0", 1, &out) == CONNECT_OK &&
	          count(A_SPAWN_DHCPC) == 1,
	      "CONNECT launches it");

	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(strcmp(find_act(A_SPAWN_DHCPC)->profile, "10-a") == 0,
	      "the first matching profile in order wins");

	reset("");
	add("999-dhcp", "*", AUTO);
	core_link(&c, "veth9", 0, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "the fallback skips virtual interfaces");
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1, "the fallback covers physical interfaces");

	reset("");
	char wifi[] =
	    "match {\n interface = tnd0\n ssid = \"casa\"\n security = open\n"
	    "}\napply {\n" AUTO "}\n";
	profile_parse(&cfg.profiles[cfg.n++], "10-wifi", wifi, strlen(wifi), 0);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "a WiFi profile does not match a cable");
}

static void test_managed(void) {
	reset("tnd0 {\n metric = 1\n");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && logged("not managed"),
	      "a broken interfaces file manages nothing");

	reset("tnd0 {\n wifi = yes\n}\n");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd1", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link(&c, "tnd1", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1 &&
	          strcmp(find_act(A_SPAWN_DHCPC)->iface, "tnd1") == 0 &&
	          logged("unknown key wifi"),
	      "an invalid block only leaves that interface unmanaged");

	reset("");
	add("10-fixed", "tnd0", STATIC);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && st("tnd0") == C_BLOCKED &&
	          logged("not implemented"),
	      "a static profile is reported, not launched");
}

static void test_supervision(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);

	out.n = 0;
	core_child_exit(&c, "tnd0", EXITED(0), 100, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && st("tnd0") == C_BLOCKED &&
	          logged("without being asked"),
	      "0 not requested: logged and not relaunched");
	core_link(&c, "tnd0", 1, 200, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "and autoconnect does not undo it");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_child_exit(&c, "tnd0", EXITED(2), 100, &out);
	CHECK(st("tnd0") == C_BLOCKED && logged("waiting for a reload"),
	      "2: not relaunched until a reload");
	CHECK(core_connect(&c, "10-a", "tnd0", 200, &out) == CONNECT_BLOCKED,
	      "2: CONNECT does not bypass it");

	reset("");
	add("10-a", "tnd0", AUTO);
	add("999-dhcp", "*", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	out.n = 0;
	core_child_exit(&c, "tnd0", EXITED(111), 100, &out);
	core_link(&c, "tnd0", 1, 200, &out);
	CHECK(st("tnd0") == C_BLOCKED && count(A_SPAWN_DHCPC) == 0,
	      "111: no profile autoconnects on that cable");
	CHECK(core_connect(&c, "10-a", "tnd0", 300, &out) == CONNECT_ALERT_PENDING,
	      "111: the same profile needs an answer to the alert");
	out.n = 0;
	CHECK(core_connect(&c, "999-dhcp", "tnd0", 400, &out) == CONNECT_OK &&
	          count(A_SPAWN_DHCPC) == 1,
	      "111: another profile can be connected on purpose");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	out.n = 0;
	core_child_exit(&c, "tnd0", 9, 100, &out);
	CHECK(st("tnd0") == C_BACKOFF && logged("killed by signal 9"),
	      "killed by a signal: like 1, logged");
}

static void test_backoff(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);

	unsigned int waits[8];
	uint64_t now = 10;
	for (int i = 0; i < 8; i++) {
		core_child_exit(&c, "tnd0", EXITED(1), now, &out);
		waits[i] = (unsigned int)(core_next_deadline(&c) - now);
		out.n = 0;
		core_timer(&c, core_next_deadline(&c) - 1, &out);
		if (i == 0)
			CHECK(count(A_SPAWN_DHCPC) == 0, "nothing before the deadline");
		now = core_next_deadline(&c);
		core_timer(&c, now, &out);
		if (i == 0)
			CHECK(count(A_SPAWN_DHCPC) == 1, "relaunched at the deadline");
	}
	CHECK(waits[0] == 1000 && waits[1] == 2000 && waits[2] == 4000 &&
	          waits[5] == 32000 && waits[6] == 60000 && waits[7] == 60000,
	      "backoff doubles from 1 s up to 60 s");

	core_child_exit(&c, "tnd0", EXITED(1), now + STABLE_MS, &out);
	CHECK(core_next_deadline(&c) == now + STABLE_MS + BACKOFF_MIN_MS,
	      "a stable run resets the backoff to 1 s");
}

static void test_link_gone(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link_gone(&c, "tnd0", 50, &out);
	core_child_exit(&c, "tnd0", EXITED(1), 60, &out);
	CHECK(st("tnd0") == C_WAIT_LINK && core_next_deadline(&c) == UINT64_MAX,
	      "1 with the interface gone waits for it");
	out.n = 0;
	core_link(&c, "tnd0", 1, 70, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1, "relaunched as soon as it is back");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_child_exit(&c, "tnd0", EXITED(1), 60, &out);
	core_link_gone(&c, "tnd0", 70, &out);
	out.n = 0;
	core_timer(&c, 5000, &out);
	CHECK(st("tnd0") == C_WAIT_LINK && count(A_SPAWN_DHCPC) == 0,
	      "the order of exit and removal does not matter");
	core_link(&c, "tnd0", 1, 5100, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1, "and it is relaunched when back");
}

static void test_user(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd0", MANUAL);
	core_link(&c, "tnd0", 1, 0, &out);

	out.n = 0;
	CHECK(core_disconnect(&c, "tnd0", 50, &out) == 0 && count(A_SIGNAL) == 1 &&
	          find_act(A_SIGNAL)->sig == SIGUSR1 && st("tnd0") == C_STOPPING,
	      "DISCONNECT releases with SIGUSR1");
	core_child_exit(&c, "tnd0", EXITED(0), 100, &out);
	core_link(&c, "tnd0", 1, 200, &out);
	CHECK(st("tnd0") == C_IDLE && count(A_SPAWN_DHCPC) == 0,
	      "after DISCONNECT nothing autoconnects");

	out.n = 0;
	core_connect(&c, "10-a", "tnd0", 300, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1, "CONNECT after DISCONNECT");
	out.n = 0;
	CHECK(core_connect(&c, "20-b", "tnd0", 400, &out) == CONNECT_OK &&
	          find_act(A_SIGNAL) && count(A_SPAWN_DHCPC) == 0,
	      "switching profile releases the current one first");
	core_child_exit(&c, "tnd0", EXITED(0), 500, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1 &&
	          strcmp(find_act(A_SPAWN_DHCPC)->profile, "20-b") == 0,
	      "and then launches the new one");

	CHECK(core_connect(&c, "nope", "tnd0", 0, &out) == CONNECT_NO_PROFILE,
	      "CONNECT to an unknown profile");
	CHECK(core_connect(&c, "10-a", "tnd9", 0, &out) == CONNECT_NO_IFACE,
	      "CONNECT on a missing interface");
	add("30-other", "tnd1", AUTO);
	CHECK(core_connect(&c, "30-other", "tnd0", 0, &out) == CONNECT_NO_MATCH,
	      "CONNECT with a profile for another interface");
	char bad[] = "match {\n}\n";
	profile_parse(&cfg.profiles[cfg.n++], "40-bad", bad, strlen(bad), 0);
	CHECK(core_connect(&c, "40-bad", "tnd0", 0, &out) == CONNECT_INVALID,
	      "CONNECT with an invalid profile");
	CHECK(core_disconnect(&c, "tnd9", 0, &out) < 0,
	      "DISCONNECT on an unknown interface");
}

static void test_shutdown(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd1", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link(&c, "tnd1", 1, 0, &out);
	core_child_exit(&c, "tnd1", EXITED(1), 10, &out);

	out.n = 0;
	core_shutdown(&c, 15, &out);
	CHECK(count(A_SIGNAL) == 1 && find_act(A_SIGNAL)->sig == SIGTERM &&
	          !core_done(&c),
	      "shutdown sends SIGTERM to every running tundra-dhcpc");
	core_timer(&c, 100000, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 20, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && core_done(&c),
	      "nothing is relaunched and the core is done");
}

static void test_stop_timeout(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd0", MANUAL);
	core_link(&c, "tnd0", 1, 0, &out);
	core_disconnect(&c, "tnd0", 1000, &out);
	CHECK(core_next_deadline(&c) == 1000 + STOP_TIMEOUT_MS,
	      "a stop has a deadline");
	out.n = 0;
	core_timer(&c, 1000 + STOP_TIMEOUT_MS - 1, &out);
	CHECK(count(A_SIGNAL) == 0, "no kill before the deadline");
	core_timer(&c, 1000 + STOP_TIMEOUT_MS, &out);
	CHECK(count(A_SIGNAL) == 1 && find_act(A_SIGNAL)->sig == SIGKILL &&
	          logged("did not stop in 5 s, killed"),
	      "SIGKILL once the 5 s are over");
	out.n = 0;
	core_timer(&c, 1000 + 2 * STOP_TIMEOUT_MS, &out);
	CHECK(count(A_SIGNAL) == 0 && core_next_deadline(&c) == UINT64_MAX,
	      "a single SIGKILL");
	core_child_exit(&c, "tnd0", SIGKILL, 7000, &out);
	CHECK(st("tnd0") == C_IDLE && count(A_SPAWN_DHCPC) == 0,
	      "the killed child ends the stop");

	out.n = 0;
	core_connect(&c, "10-a", "tnd0", 8000, &out);
	core_connect(&c, "20-b", "tnd0", 9000, &out);
	CHECK(core_next_deadline(&c) == 9000 + STOP_TIMEOUT_MS,
	      "switching profile has the same deadline");
	core_timer(&c, 9000 + STOP_TIMEOUT_MS, &out);
	core_child_exit(&c, "tnd0", SIGKILL, 9000 + STOP_TIMEOUT_MS, &out);
	CHECK(st("tnd0") == C_RUNNING &&
	          strcmp(core_find(&c, "tnd0")->profile, "20-b") == 0,
	      "and the new profile starts after the kill");

	out.n = 0;
	core_shutdown(&c, 20000, &out);
	core_timer(&c, 20000 + STOP_TIMEOUT_MS, &out);
	CHECK(count(A_SIGNAL) == 2 && out.a[out.n - 2].sig == SIGKILL,
	      "the shutdown has the same deadline");
}

int main(void) {
	test_autoconnect();
	test_managed();
	test_supervision();
	test_backoff();
	test_link_gone();
	test_user();
	test_shutdown();
	test_stop_timeout();
	return test_summary();
}
