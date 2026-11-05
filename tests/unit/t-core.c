#include "conf/grammar.c"
#include "conf/ifaces.c"
#include "conf/profile.c"
#include "core.c"
#include "test.h"

#include <arpa/inet.h>

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
	          logged("not managed: line 2: wifi on a wired interface"),
	      "wifi on a cable only leaves that interface unmanaged");
	CHECK(core_unmanaged(&c, "tnd0") &&
	          strcmp(core_unmanaged(&c, "tnd0"),
	                 "line 2: wifi on a wired interface") == 0,
	      "and STATUS gets the reason");

	reset("tnd0 {\n metric = x\n}\n");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd1", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link(&c, "tnd1", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1 &&
	          strcmp(find_act(A_SPAWN_DHCPC)->iface, "tnd1") == 0 &&
	          logged("invalid metric"),
	      "an invalid block only leaves that interface unmanaged");

	reset("");
	add("10-fixed", "tnd0", STATIC);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && count(A_SPAWN_PROBE) == 1 &&
	          st("tnd0") == C_PROBING,
	      "a static profile starts with a probe, not with dhcpc");
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

static struct config k[2];
static int cur;

#define IF100 "tnd0 {\n metric = 100\n}\n"
#define IF300 "tnd0 {\n metric = 300\n}\n"
#define DNS9                                                                   \
	" address = dhcp\n dns = 9.9.9.9\n autoconnect = yes\n trusted = no\n"
#define TRUSTED                                                                \
	" address = dhcp\n dns = 9.9.9.9\n autoconnect = yes\n trusted = yes\n"

static void build(struct config *cf, const char *ifaces, const char *iface,
                  const char *apply) {
	char buf[512];
	memset(cf, 0, sizeof(*cf));
	ifaces_parse(&cf->ifaces, ifaces, strlen(ifaces));
	if (!apply)
		return;
	snprintf(buf, sizeof(buf), "match {\n interface = %s\n}\napply {\n%s}\n",
	         iface, apply);
	profile_parse(&cf->profiles[cf->n++], "10-a", buf, strlen(buf), 0);
}

static void reload(const char *ifaces, const char *iface, const char *apply,
                   uint64_t now) {
	cur ^= 1;
	build(&k[cur], ifaces, iface, apply);
	out.n = 0;
	core_reload(&c, &k[cur], now, &out);
}

static const struct core_iface *tnd0(void) { return core_find(&c, "tnd0"); }

static void test_reload(void) {
	cur = 0;
	build(&k[0], IF100, "tnd0", AUTO);
	core_init(&c, &k[0]);
	core_link(&c, "tnd0", 1, 0, &out);

	reload(IF100, "tnd0", AUTO, 1000);
	CHECK(out.n == 0 && st("tnd0") == C_RUNNING,
	      "a reload without changes does nothing");
	reload(IF100, "tnd0", DNS9, 2000);
	CHECK(out.n == 0 && tnd0()->conn.dns == DNS_LIST,
	      "only dns: no reconnect, the new dns is taken");
	reload(IF300, "tnd0", DNS9, 3000);
	CHECK(count(A_SIGNAL) == 1 && find_act(A_SIGNAL)->sig == SIGUSR1 &&
	          strcmp(tnd0()->pending, "10-a") == 0 &&
	          logged("profile 10-a changed, reconnecting"),
	      "a change in interfaces reconnects");
	core_child_exit(&c, "tnd0", EXITED(0), 3100, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1 && find_act(A_SPAWN_DHCPC)->metric == 300,
	      "and relaunches with the new metric");
	reload(IF300, "tnd0", TRUSTED, 4000);
	CHECK(count(A_SIGNAL) == 1, "a change in the profile reconnects");
	core_child_exit(&c, "tnd0", EXITED(0), 4100, &out);

	reload(IF300, "tnd0", " address = dhcp\n", 5000);
	CHECK(count(A_SIGNAL) == 0 && st("tnd0") == C_RUNNING &&
	          logged("profile 10-a has errors, keeping the previous") &&
	          tnd0()->kept[0],
	      "a profile with errors keeps the connection");
	core_child_exit(&c, "tnd0", EXITED(1), 5100, &out);
	out.n = 0;
	core_timer(&c, 5100 + BACKOFF_MAX_MS, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1 && find_act(A_SPAWN_DHCPC)->metric == 300 &&
	          tnd0()->conn.trusted,
	      "and a relaunch uses the previous configuration");
	reload("tnd0 {\n wifi = yes\n}\n", "tnd0", TRUSTED, 6000);
	CHECK(count(A_SIGNAL) == 0 && logged("interfaces has errors, keeping"),
	      "an error in interfaces keeps the connection");
	reload(IF300, "tnd0", TRUSTED, 7000);
	CHECK(out.n == 0 && !tnd0()->kept[0],
	      "fixing the errors with the same configuration changes nothing");

	reload(IF300, "tnd1", TRUSTED, 8000);
	CHECK(count(A_SIGNAL) == 1 && !tnd0()->pending[0] &&
	          logged("no longer matches, disconnecting"),
	      "a profile moved to another interface disconnects");
	core_child_exit(&c, "tnd0", EXITED(0), 8100, &out);
	reload(IF300, "tnd0", TRUSTED, 9000);
	CHECK(count(A_SPAWN_DHCPC) == 1, "a new profile autoconnects on reload");

	reload(IF300, "tnd0", NULL, 10000);
	CHECK(count(A_SIGNAL) == 1 && !tnd0()->pending[0] &&
	          logged("profile 10-a removed, disconnecting"),
	      "a deleted profile disconnects");
	core_child_exit(&c, "tnd0", EXITED(0), 10100, &out);
	CHECK(st("tnd0") == C_IDLE && count(A_SPAWN_DHCPC) == 0,
	      "and nothing takes its place");

	reload(IF300, "tnd0", TRUSTED, 11000);
	core_child_exit(&c, "tnd0", EXITED(2), 11100, &out);
	reload(IF300, "tnd0", TRUSTED, 12000);
	CHECK(count(A_SPAWN_DHCPC) == 1, "a reload unblocks a rejected profile");
	core_child_exit(&c, "tnd0", EXITED(111), 12100, &out);
	reload(IF300, "tnd0", TRUSTED, 13000);
	CHECK(count(A_SPAWN_DHCPC) == 0 && st("tnd0") == C_BLOCKED,
	      "but not one stopped by an alert");
}

static const struct action *sensor_act(int sig) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == A_SIGNAL && out.a[i].kind == K_SENSOR &&
		    out.a[i].sig == sig)
			return &out.a[i];
	return NULL;
}

static const struct action *dhcpc_sig(int sig) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == A_SIGNAL && out.a[i].kind == K_DHCPC &&
		    out.a[i].sig == sig)
			return &out.a[i];
	return NULL;
}

static void test_sensor(void) {
	uint32_t a200 = inet_addr("10.79.0.200"), a201 = inet_addr("10.79.0.201");
	uint32_t a202 = inet_addr("10.79.0.202"), gw = inet_addr("10.79.0.1");
	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);

	out.n = 0;
	core_lease(&c, "tnd0", a200, gw, 100, &out);
	CHECK(count(A_SPAWN_SENSOR) == 1 &&
	          find_act(A_SPAWN_SENSOR)->addr == a200 &&
	          find_act(A_SPAWN_SENSOR)->gw == gw,
	      "a lease launches the sensor with its address and gateway");
	out.n = 0;
	core_lease(&c, "tnd0", a200, gw, 200, &out);
	CHECK(out.n == 0, "the same lease changes nothing");
	core_lease(&c, "tnd0", a201, gw, 300, &out);
	CHECK(sensor_act(SIGTERM) && count(A_SPAWN_SENSOR) == 0,
	      "a new address stops the sensor");
	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(0), NULL, 400, &out);
	CHECK(count(A_SPAWN_SENSOR) == 1 && find_act(A_SPAWN_SENSOR)->addr == a201,
	      "and relaunches it on the new one");
	out.n = 0;
	core_lease(&c, "tnd0", 0, 0, 500, &out);
	core_sensor_exit(&c, "tnd0", EXITED(0), NULL, 600, &out);
	CHECK(sensor_act(SIGTERM) && count(A_SPAWN_SENSOR) == 0 &&
	          tnd0()->sst == S_OFF,
	      "losing the lease stops the sensor");

	core_lease(&c, "tnd0", a202, gw, 700, &out);
	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(1), "02:aa:aa:aa:aa:01", 800, &out);
	CHECK(dhcpc_sig(SIGUSR2) && count(A_SPAWN_SENSOR) == 0,
	      "a conflict asks tundra-dhcpc for another address");
	out.n = 0;
	core_lease(&c, "tnd0", a202, gw, 900, &out);
	CHECK(count(A_SPAWN_SENSOR) == 0,
	      "the sensor does not come back on the conflicting address");
	core_lease(&c, "tnd0", a200, gw, 1000, &out);
	CHECK(count(A_SPAWN_SENSOR) == 1, "but does on the next one");

	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 2000, &out);
	CHECK(tnd0()->sst == S_BACKOFF &&
	          logged("relaunching tundra-blueice-arp in 1000 ms"),
	      "a failing sensor is relaunched with backoff");
	CHECK(core_next_deadline(&c) == 3000, "the backoff is a deadline");
	core_timer(&c, 3000, &out);
	CHECK(count(A_SPAWN_SENSOR) == 1, "and relaunched when it expires");
	core_sensor_exit(&c, "tnd0", SIGSEGV, NULL, 3100, &out);
	core_timer(&c, 5100, &out);
	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(2), NULL, 5200, &out);
	CHECK(logged("failed 3 times in a row") && find_act(A_ALERT) &&
	          find_act(A_ALERT)->alert == AL_STOP &&
	          find_act(A_ALERT)->level == 5 &&
	          strcmp(find_act(A_ALERT)->kase, "sensor-down") == 0 &&
	          tnd0()->sst == S_OFF,
	      "the third failure in a row raises a sensor-down stop");
	out.n = 0;
	core_stop(&c, "tnd0", 5, 1, 5200, &out);
	CHECK(dhcpc_sig(SIGUSR1) && logged("stopped by an alert of level 5") &&
	          tnd0()->override == 0,
	      "which stops the connection without lowering trust");
	core_child_exit(&c, "tnd0", EXITED(0), 5300, &out);
	CHECK(st("tnd0") == C_BLOCKED && tnd0()->blocked == B_ALERT,
	      "and leaves the interface stopped");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_lease(&c, "tnd0", a200, gw, 0, &out);
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 100, &out);
	core_timer(&c, 1100, &out);
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 1100 + STABLE_MS, &out);
	CHECK(tnd0()->s_fails == 1 &&
	          logged("relaunching tundra-blueice-arp in 1000 ms"),
	      "a sensor that lasted 60 s starts counting again");
}

static void test_stop(void) {
	uint32_t a200 = inet_addr("10.79.0.200"), gw = inet_addr("10.79.0.1");
	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd1", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link(&c, "tnd1", 1, 0, &out);
	core_lease(&c, "tnd0", a200, gw, 0, &out);

	out.n = 0;
	core_stop(&c, "tnd0", 8, 1, 100, &out);
	CHECK(dhcpc_sig(SIGUSR1) && sensor_act(SIGTERM) && count(A_SIGNAL) == 2 &&
	          st("tnd1") == C_RUNNING,
	      "level 8 releases only that interface");
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	core_sensor_exit(&c, "tnd0", EXITED(111), NULL, 200, &out);
	CHECK(st("tnd0") == C_BLOCKED && tnd0()->blocked == B_ALERT &&
	          count(A_SPAWN_SENSOR) == 0,
	      "and leaves it stopped, without a sensor");
	out.n = 0;
	core_lease(&c, "tnd0", a200, gw, 300, &out);
	CHECK(count(A_SPAWN_SENSOR) == 0, "a stale lease brings no sensor back");
	CHECK(core_connect(&c, "10-a", "tnd0", 400, &out) == CONNECT_ALERT_PENDING,
	      "the stopped profile cannot be connected again");

	out.n = 0;
	core_stop(&c, "tnd1", 9, 2, 500, &out);
	CHECK(dhcpc_sig(SIGUSR1) && c.lockdown &&
	          logged("stopped by an alert of level 9 on tnd1"),
	      "level 9 stops every interface");
	core_child_exit(&c, "tnd1", EXITED(0), 600, &out);
	core_link(&c, "tnd2", 1, 700, &out);
	CHECK(st("tnd1") == C_BLOCKED && core_find(&c, "tnd2")->st == C_BLOCKED,
	      "including the ones that appear later");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_lease(&c, "tnd0", a200, gw, 0, &out);
	out.n = 0;
	core_shutdown(&c, 100, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	CHECK(sensor_act(SIGTERM) && !core_done(&c),
	      "the shutdown waits for the sensor");
	core_sensor_exit(&c, "tnd0", EXITED(0), NULL, 300, &out);
	CHECK(core_done(&c) && count(A_SPAWN_SENSOR) == 0,
	      "and is done once it exits");
}

static void connect_both(void) {
	reset("");
	add("10-a", "tnd0", AUTO);
	add("20-b", "tnd1", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_link(&c, "tnd1", 1, 0, &out);
}

static const struct core_iface *ifc(const char *name) {
	return core_find(&c, name);
}

static void test_alerts(void) {
	uint32_t a200 = inet_addr("10.79.0.200"), gw = inet_addr("10.79.0.1");
	connect_both();
	core_lease(&c, "tnd0", a200, gw, 0, &out);
	core_note_alert(&c, "tnd0", 8, 7);
	core_stop(&c, "tnd0", 8, 7, 100, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	core_sensor_exit(&c, "tnd0", EXITED(111), NULL, 200, &out);
	CHECK(ifc("tnd0")->stop_id == 7 && ifc("tnd0")->override == 7,
	      "a stop of level 8 ties the interface to its alert and lowers trust");
	out.n = 0;
	CHECK(core_force(&c, 7, "tnd0", "D", 8, 300, &out) == 1 &&
	          count(A_SPAWN_DHCPC) == 1 && !find_act(A_SPAWN_DHCPC)->probe_off,
	      "force relaunches tundra-dhcpc for the same profile");
	out.n = 0;
	core_lease(&c, "tnd0", a200, gw, 400, &out);
	CHECK(count(A_SPAWN_SENSOR) == 1 &&
	          strcmp(find_act(A_SPAWN_SENSOR)->accept, "D/8") == 0 &&
	          !find_act(A_SPAWN_SENSOR)->defend_always,
	      "and the sensor with accept D/8");
	CHECK(ifc("tnd0")->override == 7, "the forced connection stays untrusted");
	core_stop(&c, "tnd0", 6, 8, 500, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 600, &out);
	core_sensor_exit(&c, "tnd0", SIGTERM, NULL, 600, &out);
	core_force(&c, 8, "tnd0", "C", 6, 700, &out);
	CHECK(ifc("tnd0")->naccepts == 2, "accepts pile up within the connection");
	core_disconnect(&c, "tnd0", 740, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 750, &out);
	core_connect(&c, "10-a", "tnd0", 760, &out);
	CHECK(ifc("tnd0")->naccepts == 0 && ifc("tnd0")->override == 0,
	      "a new connection starts without accepts or distrust");
	core_lease(&c, "tnd0", a200, gw, 800, &out);
	core_stop(&c, "tnd0", 6, 9, 900, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 950, &out);
	core_sensor_exit(&c, "tnd0", SIGTERM, NULL, 950, &out);
	out.n = 0;
	core_force(&c, 9, "tnd0", "C", 6, 1000, &out);
	CHECK(find_act(A_SPAWN_DHCPC) && find_act(A_SPAWN_DHCPC)->probe_off,
	      "forcing a dispute relaunches dhcpc with probe off");
	out.n = 0;
	core_lease(&c, "tnd0", a200, gw, 1100, &out);
	CHECK(find_act(A_SPAWN_SENSOR) && find_act(A_SPAWN_SENSOR)->defend_always &&
	          strcmp(find_act(A_SPAWN_SENSOR)->accept, "C/6") == 0,
	      "and the sensor with defend always");
	CHECK(core_force(&c, 9, "tnd0", "C", 6, 1200, &out) == 0,
	      "an alert can only be answered once");

	connect_both();
	core_stop(&c, "tnd0", 8, 3, 100, &out);
	out.n = 0;
	CHECK(core_force(&c, 3, "tnd0", "D", 8, 150, &out) == 1 &&
	          count(A_SPAWN_DHCPC) == 0 &&
	          strcmp(ifc("tnd0")->pending, "10-a") == 0,
	      "force before dhcpc has stopped waits for it");
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	CHECK(count(A_SPAWN_DHCPC) == 1, "and relaunches it once it has");

	connect_both();
	core_stop(&c, "tnd0", 8, 4, 100, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	out.n = 0;
	CHECK(core_release_alert(&c, 4, &out) == 1 && st("tnd0") == C_IDLE &&
	          ifc("tnd0")->manual_off && ifc("tnd0")->stop_id == 0,
	      "disconnect leaves the interface as after DISCONNECT");
	core_link(&c, "tnd0", 1, 300, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0, "so nothing autoconnects there");

	connect_both();
	core_stop(&c, "tnd0", 9, 5, 100, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	core_child_exit(&c, "tnd1", EXITED(0), 200, &out);
	out.n = 0;
	CHECK(
	    core_force(&c, 5, "tnd0", "X", 9, 300, &out) >= 2 &&
	        count(A_SPAWN_DHCPC) == 2 && !c.lockdown &&
	        ifc("tnd0")->naccepts == 1 && ifc("tnd1")->naccepts == 0 &&
	        ifc("tnd1")->override == 5,
	    "force on level 9 brings every connection back, accept on the origin");

	connect_both();
	core_stop(&c, "tnd0", 9, 6, 100, &out);
	core_child_exit(&c, "tnd0", EXITED(0), 200, &out);
	core_child_exit(&c, "tnd1", EXITED(0), 200, &out);
	CHECK(core_release_alert(&c, 6, &out) >= 2 && st("tnd1") == C_IDLE &&
	          ifc("tnd1")->manual_off && !c.lockdown,
	      "disconnect on level 9 releases every interface");

	connect_both();
	core_note_alert(&c, "tnd1", 6, 11);
	CHECK(ifc("tnd1")->override == 11, "a warning of level 6 lowers trust");
	core_note_alert(&c, "tnd1", 7, 12);
	CHECK(ifc("tnd1")->override == 11, "the first one is the one shown");
	core_note_alert(&c, "tnd0", 5, 13);
	CHECK(ifc("tnd0")->override == 0, "level 5 does not");
	core_disconnect(&c, "tnd1", 100, &out);
	CHECK(ifc("tnd1")->override == 0,
	      "DISCONNECT restores the profile's trust");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_restore_stop(&c, "tnd0", "10-a", 8, 21);
	out.n = 0;
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(st("tnd0") == C_BLOCKED && count(A_SPAWN_DHCPC) == 0 &&
	          strcmp(ifc("tnd0")->conn.name, "10-a") == 0 &&
	          ifc("tnd0")->override == 21,
	      "a stop restored after a restart keeps the interface stopped");
	CHECK(core_connect(&c, "10-a", "tnd0", 10, &out) == CONNECT_ALERT_PENDING,
	      "and its profile cannot be connected");
	CHECK(core_force(&c, 21, "tnd0", "D", 8, 20, &out) == 1 &&
	          count(A_SPAWN_DHCPC) == 1,
	      "until the alert is forced");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_restore_stop(&c, "tnd9", "-", 9, 22);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(st("tnd0") == C_BLOCKED && ifc("tnd0")->stop_id == 22,
	      "a restored level 9 blocks every interface that appears");

	reset("");
	add("10-a", "tnd0", AUTO);
	core_link(&c, "tnd0", 1, 0, &out);
	core_lease(&c, "tnd0", a200, gw, 0, &out);
	struct core_iface *ci = (struct core_iface *)ifc("tnd0");
	ci->naccepts = 1;
	snprintf(ci->accepts[0].kase, sizeof(ci->accepts[0].kase), "sensor-down");
	ci->accepts[0].level = 5;
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 10, &out);
	core_timer(&c, 1010, &out);
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 1020, &out);
	core_timer(&c, 3020, &out);
	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 3030, &out);
	CHECK(find_act(A_ALERT) && find_act(A_ALERT)->alert == AL_WARN &&
	          ci->sst == S_BACKOFF && ci->s_fails == 0,
	      "a forced sensor-down only warns and keeps relaunching");
}

#define STATIC_GW                                                              \
	" address = 192.168.1.50/24\n gateway = 192.168.1.1\n dns = 9.9.9.9\n"     \
	" autoconnect = yes\n trusted = no\n"

static const struct action *kind_sig(enum child_kind kind, int sig) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == A_SIGNAL && out.a[i].kind == kind &&
		    out.a[i].sig == sig)
			return &out.a[i];
	return NULL;
}

static void probe_free(uint64_t now) {
	core_probe_exit(&c, "tnd0", EXITED(0), PROBE_FREE, NULL, NULL, now, &out);
}

static void test_static(void) {
	uint32_t ip = inet_addr("192.168.1.50"), gw = inet_addr("192.168.1.1");
	reset("tnd0 {\n metric = 300\n table = 100\n}\n");
	add("10-fixed", "tnd0", STATIC_GW);
	add("20-dhcp", "tnd0", MANUAL);
	core_link(&c, "tnd0", 1, 0, &out);
	CHECK(find_act(A_SPAWN_PROBE) && find_act(A_SPAWN_PROBE)->addr == ip &&
	          st("tnd0") == C_PROBING,
	      "a static profile probes its address first");
	out.n = 0;
	probe_free(100);
	const struct action *a = find_act(A_STATIC_APPLY);
	CHECK(a && a->addr == ip && a->prefix == 24 && a->gw == gw &&
	          a->has_metric && a->metric == 300 && a->has_table &&
	          strcmp(a->table, "100") == 0 && st("tnd0") == C_STATIC,
	      "a free address is applied with metric and table");
	CHECK(count(A_SPAWN_SENSOR) == 0, "the sensor waits for tundra-ip");
	out.n = 0;
	core_static_result(&c, "tnd0", 0, 110, &out);
	CHECK(find_act(A_SPAWN_SENSOR) && find_act(A_SPAWN_SENSOR)->addr == ip &&
	          find_act(A_SPAWN_SENSOR)->gw == gw,
	      "and then watched");
	out.n = 0;
	core_lease(&c, "tnd0", 0, 0, 120, &out);
	CHECK(out.n == 0, "lease events do not touch a static connection");

	out.n = 0;
	core_disconnect(&c, "tnd0", 200, &out);
	CHECK(find_act(A_STATIC_REMOVE) && kind_sig(K_SENSOR, SIGTERM) &&
	          st("tnd0") == C_IDLE,
	      "DISCONNECT removes it at once");
	out.n = 0;
	core_connect(&c, "10-fixed", "tnd0", 300, &out);
	CHECK(find_act(A_SPAWN_PROBE), "CONNECT probes again");
	out.n = 0;
	core_disconnect(&c, "tnd0", 310, &out);
	CHECK(kind_sig(K_PROBE, SIGTERM) && st("tnd0") == C_STOPPING,
	      "DISCONNECT during the probe stops the probe");
	core_probe_exit(&c, "tnd0", SIGTERM, PROBE_NONE, NULL, NULL, 320, &out);
	CHECK(st("tnd0") == C_IDLE, "and ends idle when it exits");

	core_connect(&c, "10-fixed", "tnd0", 400, &out);
	out.n = 0;
	core_probe_exit(&c, "tnd0", EXITED(0), PROBE_TAKEN, "02:aa:aa:aa:aa:01",
	                "02:bb:bb:bb:bb:bb", 500, &out);
	a = find_act(A_ALERT);
	CHECK(a && a->alert == AL_WARN && a->level == 2 &&
	          strcmp(a->kase, "static-taken") == 0 &&
	          strcmp(a->msg, "ip 192.168.1.50 mac 02:aa:aa:aa:aa:01 src "
	                         "02:bb:bb:bb:bb:bb") == 0 &&
	          !find_act(A_STATIC_APPLY),
	      "a taken address is not applied and raises static-taken");
	CHECK(st("tnd0") == C_BLOCKED && ifc("tnd0")->blocked == B_TAKEN,
	      "and leaves the interface blocked");
	out.n = 0;
	core_timer(&c, 500 + BACKOFF_MAX_MS, &out);
	core_reload(&c, &cfg, 600, &out);
	CHECK(count(A_SPAWN_PROBE) == 0 && st("tnd0") == C_BLOCKED,
	      "without retrying, not even on reload");
	CHECK(core_connect(&c, "10-fixed", "tnd0", 700, &out) == CONNECT_OK &&
	          find_act(A_SPAWN_PROBE),
	      "until a CONNECT");

	out.n = 0;
	core_probe_exit(&c, "tnd0", EXITED(1), PROBE_NONE, NULL, NULL, 800, &out);
	CHECK(st("tnd0") == C_BACKOFF &&
	          logged("retrying the static address in 1000 ms"),
	      "a failing probe is retried with backoff");
	out.n = 0;
	core_timer(&c, 1800, &out);
	CHECK(find_act(A_SPAWN_PROBE), "and probes again");
	probe_free(1900);
	out.n = 0;
	core_static_result(&c, "tnd0", STATIC_FAIL, 1950, &out);
	CHECK(st("tnd0") == C_BACKOFF && count(A_SPAWN_SENSOR) == 0,
	      "a failure of tundra-ip is retried too");
	core_timer(&c, 1950 + BACKOFF_MAX_MS, &out);
	probe_free(70000);
	core_static_result(&c, "tnd0", STATIC_USAGE, 70010, &out);
	CHECK(st("tnd0") == C_BLOCKED && ifc("tnd0")->blocked == B_CONFIG,
	      "a usage error of tundra-ip waits for a reload");

	reset("");
	add("10-fixed", "tnd0", STATIC_GW);
	add("20-dhcp", "tnd0", MANUAL);
	core_link(&c, "tnd0", 1, 0, &out);
	probe_free(10);
	core_static_result(&c, "tnd0", 0, 20, &out);
	out.n = 0;
	core_sensor_exit(&c, "tnd0", EXITED(1), "02:aa:aa:aa:aa:01", 30, &out);
	a = find_act(A_ALERT);
	CHECK(find_act(A_STATIC_REMOVE) && a &&
	          strcmp(a->msg, "ip 192.168.1.50 mac 02:aa:aa:aa:aa:01") == 0 &&
	          ifc("tnd0")->blocked == B_TAKEN && !kind_sig(K_DHCPC, SIGUSR2),
	      "the sensor giving the address up removes it and warns");

	core_connect(&c, "10-fixed", "tnd0", 100, &out);
	probe_free(110);
	core_static_result(&c, "tnd0", 0, 120, &out);
	out.n = 0;
	core_stop(&c, "tnd0", 6, 1, 200, &out);
	CHECK(find_act(A_STATIC_REMOVE) && st("tnd0") == C_BLOCKED &&
	          ifc("tnd0")->blocked == B_ALERT,
	      "a stop removes the static address");
	core_sensor_exit(&c, "tnd0", EXITED(111), NULL, 210, &out);
	out.n = 0;
	core_force(&c, 1, "tnd0", "C", 6, 300, &out);
	CHECK(find_act(A_STATIC_APPLY) && !find_act(A_SPAWN_PROBE),
	      "forcing a dispute applies it without probing");
	core_static_result(&c, "tnd0", 0, 310, &out);
	CHECK(find_act(A_SPAWN_SENSOR) && find_act(A_SPAWN_SENSOR)->defend_always,
	      "and watches it with defend always");

	out.n = 0;
	core_link_gone(&c, "tnd0", 400, &out);
	CHECK(st("tnd0") == C_WAIT_LINK && kind_sig(K_SENSOR, SIGTERM),
	      "a removed interface takes the address with it");
	core_sensor_exit(&c, "tnd0", EXITED(1), NULL, 410, &out);
	out.n = 0;
	core_link(&c, "tnd0", 1, 500, &out);
	CHECK(find_act(A_STATIC_APPLY), "and comes back when it returns");
	core_static_result(&c, "tnd0", 0, 510, &out);

	out.n = 0;
	core_connect(&c, "20-dhcp", "tnd0", 600, &out);
	CHECK(find_act(A_STATIC_REMOVE) && find_act(A_SPAWN_DHCPC) &&
	          st("tnd0") == C_RUNNING,
	      "switching to a DHCP profile removes the address first");

	reset("");
	add("10-fixed", "tnd0", STATIC_GW);
	core_link(&c, "tnd0", 1, 0, &out);
	probe_free(10);
	core_static_result(&c, "tnd0", 0, 20, &out);
	out.n = 0;
	core_shutdown(&c, 30, &out);
	core_sensor_exit(&c, "tnd0", EXITED(0), NULL, 40, &out);
	CHECK(!find_act(A_STATIC_REMOVE) && core_done(&c),
	      "the shutdown leaves the static address in place");
}

#define WL (LINK_PHYSICAL | LINK_WIRELESS)
#define WL_UP (WL | LINK_UP)

static enum wpa_state wst(const char *ifname) {
	return core_find(&c, ifname)->wst;
}

static const struct action *wpa_signal(void) {
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == A_SIGNAL && out.a[i].kind == K_WPA)
			return &out.a[i];
	return NULL;
}

static void test_wpa(void) {
	reset("");
	core_link(&c, "wl0", WL_UP, 0, &out);
	const struct action *a = find_act(A_SPAWN_WPA);
	CHECK(a && a->kind == K_WPA && strcmp(a->iface, "wl0") == 0 &&
	          wst("wl0") == W_RUNNING,
	      "a wireless interface gets its wpa_supplicant");
	out.n = 0;
	core_link(&c, "wl0", WL, 10, &out);
	CHECK(count(A_SPAWN_WPA) == 0 && count(A_LINK_DOWN) == 0,
	      "a second link event does not launch another one");
	core_link(&c, "tnd0", LINK_PHYSICAL | LINK_UP, 10, &out);
	CHECK(count(A_SPAWN_WPA) == 0, "a cable does not get one");

	out.n = 0;
	core_wpa_exit(&c, "wl0", EXITED(0), 100, &out);
	CHECK(wst("wl0") == W_BACKOFF && logged("wpa_supplicant exited with 0") &&
	          logged("relaunching wpa_supplicant in 1000 ms") &&
	          core_next_deadline(&c) == 1100,
	      "an unasked exit, even with 0, is relaunched with backoff");
	out.n = 0;
	core_timer(&c, 1099, &out);
	CHECK(count(A_SPAWN_WPA) == 0, "not before its time");
	core_timer(&c, 1100, &out);
	CHECK(count(A_SPAWN_WPA) == 1 && wst("wl0") == W_RUNNING,
	      "and relaunched when it comes");
	out.n = 0;
	core_wpa_exit(&c, "wl0", 9, 1200, &out);
	CHECK(logged("wpa_supplicant killed by signal 9") &&
	          logged("relaunching wpa_supplicant in 2000 ms"),
	      "a kill counts the same, and the backoff doubles");
	core_timer(&c, 3200, &out);
	out.n = 0;
	core_wpa_exit(&c, "wl0", EXITED(1), 3200 + STABLE_MS, &out);
	CHECK(logged("relaunching wpa_supplicant in 1000 ms"),
	      "after a stable minute the backoff starts over");
	core_timer(&c, 4200 + STABLE_MS, &out);

	out.n = 0;
	core_link_gone(&c, "wl0", 70000, &out);
	CHECK(wpa_signal() && wpa_signal()->sig == SIGTERM &&
	          wst("wl0") == W_STOPPING,
	      "a removed interface stops its wpa_supplicant");
	out.n = 0;
	core_timer(&c, 70000 + STOP_TIMEOUT_MS, &out);
	CHECK(wpa_signal() && wpa_signal()->sig == SIGKILL &&
	          logged("wpa_supplicant did not stop in 5 s, killed"),
	      "and kills it if it does not stop");
	out.n = 0;
	core_wpa_exit(&c, "wl0", 9, 76000, &out);
	CHECK(wst("wl0") == W_OFF && count(A_SPAWN_WPA) == 0 && out.n == 0,
	      "an asked exit is not relaunched");
	core_link(&c, "wl0", WL, 77000, &out);
	CHECK(count(A_SPAWN_WPA) == 1, "the interface comes back with a new one");

	out.n = 0;
	core_wpa_exit(&c, "wl0", EXITED(1), 78000, &out);
	core_link_gone(&c, "wl0", 78100, &out);
	CHECK(wst("wl0") == W_OFF && core_next_deadline(&c) == UINT64_MAX,
	      "a removed interface cancels the relaunch");

	reset("");
	core_link(&c, "wl0", WL, 0, &out);
	out.n = 0;
	core_shutdown(&c, 10, &out);
	CHECK(wpa_signal() && wpa_signal()->sig == SIGTERM && !core_done(&c),
	      "shutdown stops wpa_supplicant and waits for it");
	core_wpa_exit(&c, "wl0", EXITED(0), 20, &out);
	CHECK(core_done(&c) && count(A_SPAWN_WPA) == 0, "then it is done");
}

static void test_wifi_off(void) {
	reset("wl0 {\n wifi = no\n}\n");
	add("10-a", "wl0", AUTO);
	core_link(&c, "wl0", WL_UP, 0, &out);
	CHECK(count(A_SPAWN_WPA) == 0 && count(A_SPAWN_DHCPC) == 0 &&
	          core_wifi_off(&c, "wl0"),
	      "wifi = no: no wpa_supplicant and no connection");
	CHECK(count(A_LINK_DOWN) == 1 &&
	          logged("wifi is off, bringing the interface down"),
	      "and the interface goes down");
	out.n = 0;
	core_link(&c, "wl0", WL, 10, &out);
	CHECK(out.n == 0, "once down, nothing more");
	core_link(&c, "wl0", WL_UP, 20, &out);
	CHECK(count(A_LINK_DOWN) == 1, "brought up by hand, it goes down again");
	out.n = 0;
	CHECK(core_connect(&c, "10-a", "wl0", 30, &out) == CONNECT_OFF &&
	          out.n == 0,
	      "CONNECT says the wifi is off");

	reset("tnd0 {\n wifi = no\n}\n");
	core_link(&c, "tnd0", LINK_PHYSICAL | LINK_UP, 0, &out);
	CHECK(count(A_LINK_DOWN) == 0 && !core_wifi_off(&c, "tnd0"),
	      "a cable with wifi = no is unmanaged, not brought down");

	cur = 0;
	build(&k[0], "", "wl0", AUTO);
	core_init(&c, &k[0]);
	out.n = 0;
	core_link(&c, "wl0", WL_UP, 0, &out);
	CHECK(count(A_SPAWN_WPA) == 1 && count(A_SPAWN_DHCPC) == 1,
	      "a profile without ssid connects on a wireless interface");
	reload("wl0 {\n wifi = no\n}\n", "wl0", AUTO, 1000);
	CHECK(logged("wifi is off, disconnecting") && count(A_SIGNAL) == 2 &&
	          wpa_signal() && wpa_signal()->sig == SIGTERM &&
	          count(A_LINK_DOWN) == 1,
	      "a reload to wifi = no disconnects, stops wpa_supplicant and "
	      "brings the interface down");
	out.n = 0;
	core_child_exit(&c, "wl0", EXITED(0), 1100, &out);
	core_wpa_exit(&c, "wl0", EXITED(0), 1100, &out);
	CHECK(count(A_SPAWN_DHCPC) == 0 && count(A_SPAWN_WPA) == 0 &&
	          st("wl0") == C_IDLE && wst("wl0") == W_OFF,
	      "and nothing comes back");
	reload("wl0 {\n metric = x\n}\n", "wl0", AUTO, 2000);
	CHECK(count(A_SPAWN_WPA) == 0 && count(A_SPAWN_DHCPC) == 0,
	      "an invalid block does not turn it back on");
	reload("", "wl0", AUTO, 3000);
	CHECK(count(A_SPAWN_WPA) == 1 && count(A_SPAWN_DHCPC) == 1 &&
	          !core_wifi_off(&c, "wl0"),
	      "a reload to wifi = yes launches both again");
	reload("wl0 {\n metric = x\n}\n", "wl0", AUTO, 4000);
	CHECK(!wpa_signal() && wst("wl0") == W_RUNNING,
	      "an invalid block keeps wpa_supplicant running");
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
	test_reload();
	test_sensor();
	test_stop();
	test_alerts();
	test_static();
	test_wpa();
	test_wifi_off();
	return test_summary();
}
