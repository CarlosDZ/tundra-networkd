#include "conf/grammar.c"
#include "conf/ifaces.c"
#include "conf/profile.c"
#include "core.c"
#include "proto.c"
#include "test.h"

static struct cmd c;

static int parsed(const char *line, enum cmd_type t, int nargs) {
	proto_parse(line, &c);
	return c.type == t && c.nargs == nargs;
}

static int bad(const char *line, const char *error) {
	proto_parse(line, &c);
	return c.type == CMD_BAD && strcmp(c.error, error) == 0;
}

static const char *kept;
static unsigned int override;
static const char *alerts;

static int status(struct core_iface *ci, const struct profile *p, int conn,
                  const char *addr, const char *unmanaged, const char *want) {
	char out[512];
	struct iface_view v = {ci,        p,    conn,     addr,
	                       unmanaged, kept, override, alerts};
	return proto_status(&v, out, sizeof(out)) >= 0 && strcmp(out, want) == 0;
}

int main(void) {
	CHECK(parsed("STATUS", CMD_STATUS, 0), "STATUS");
	CHECK(parsed("  PROFILES  ", CMD_PROFILES, 0), "spaces around a command");
	CHECK(parsed("CONNECT casa", CMD_CONNECT, 1) &&
	          strcmp(c.args[0], "casa") == 0,
	      "CONNECT with a profile");
	CHECK(parsed("CONNECT 999-dhcp\tenp8s0", CMD_CONNECT, 2) &&
	          strcmp(c.args[1], "enp8s0") == 0,
	      "CONNECT with an interface, tab separated");
	CHECK(parsed("", CMD_EMPTY, 0) && parsed("   ", CMD_EMPTY, 0),
	      "an empty line");
	CHECK(bad("status", "ERROR unknown command"), "commands are upper case");
	CHECK(bad("CONNECT", "ERROR usage: CONNECT <profile> [interface]"),
	      "CONNECT without a profile");
	CHECK(bad("CONNECT a b c", "ERROR usage: CONNECT <profile> [interface]"),
	      "CONNECT with too many arguments");
	CHECK(bad("DISCONNECT", "ERROR usage: DISCONNECT <interface>"),
	      "DISCONNECT without an interface");
	CHECK(bad("ALERT-REPLY 3", "ERROR usage: ALERT-REPLY <id> <option>"),
	      "ALERT-REPLY needs two arguments");
	char longarg[128] = "CONNECT ";
	memset(longarg + 8, 'a', 70);
	CHECK(bad(longarg, "ERROR usage: CONNECT <profile> [interface]"),
	      "an argument longer than a profile name");

	struct profile p = {.name = "casa",
	                    .interface = "enp8s0",
	                    .valid = 1,
	                    .autoconnect = 1,
	                    .trusted = 0};
	struct core_iface ci = {.name = "enp8s0", .st = C_RUNNING};
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted no override - alert -"),
	      "a connected interface");
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state connecting profile casa address - "
	             "trusted no override - alert -"),
	      "a connection without lease");
	kept = "profile casa has errors";
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted no override - alert - -- previous "
	             "configuration kept: "
	             "profile casa has errors"),
	      "a connection kept by a reload says so");
	kept = NULL;
	ci.st = C_IDLE;
	CHECK(status(&ci, NULL, 0, NULL, NULL,
	             "iface enp8s0 state idle profile - address - trusted - "
	             "override - alert -"),
	      "an idle interface");
	CHECK(status(&ci, NULL, 0, NULL, "interfaces: line 2: unknown key wifi",
	             "iface enp8s0 state unmanaged profile - address - trusted - "
	             "override - alert - "
	             "-- interfaces: line 2: unknown key wifi"),
	      "an unmanaged interface carries its error");
	ci.st = C_BLOCKED;
	ci.blocked = B_CONFIG;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state blocked reason config profile casa "
	             "address - trusted no override - alert -"),
	      "a blocked interface carries its reason");
	ci.st = C_BACKOFF;
	CHECK(status(&ci, &p, 0, NULL, "x",
	             "iface enp8s0 state retrying profile casa address - "
	             "trusted no override - alert -"),
	      "retrying, and the unmanaged error only shows when idle");
	ci.st = C_WAIT_LINK;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state waiting profile casa address - "
	             "trusted no override - alert -"),
	      "waiting for the interface");
	ci.st = C_STOPPING;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state stopping profile casa address - "
	             "trusted no override - alert -"),
	      "stopping");

	ci.st = C_RUNNING;
	override = 7;
	alerts = "7,9";
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted no override 7 alert 7,9"),
	      "an alert lowers trust and pending alerts are listed");
	p.trusted = 1;
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted no override 7 alert 7,9"),
	      "even on a trusted profile");
	override = 0;
	alerts = NULL;
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted yes override - alert -"),
	      "a trusted profile without alerts");
	p.trusted = 0;

	char out[512];
	CHECK(proto_profile(&p, out, sizeof(out)) >= 0 &&
	          strcmp(out, "profile casa interface enp8s0 autoconnect yes "
	                      "trusted no") == 0,
	      "a profile line");
	struct profile any = {
	    .name = "999-dhcp", .any_interface = 1, .valid = 1, .autoconnect = 1};
	CHECK(proto_profile(&any, out, sizeof(out)) >= 0 &&
	          strstr(out, "interface * "),
	      "the wildcard profile");
	struct profile broken = {.name = "roto", .error = "missing apply section"};
	CHECK(proto_profile(&broken, out, sizeof(out)) >= 0 &&
	          strcmp(out, "profile roto invalid -- missing apply section") == 0,
	      "an invalid profile carries its error");
	CHECK(proto_event(1, "enp8s0", "casa", "10.79.0.100/24", out,
	                  sizeof(out)) >= 0 &&
	          strcmp(out, "event connected enp8s0 casa address "
	                      "10.79.0.100/24") == 0,
	      "event connected");
	CHECK(proto_event(0, "enp8s0", "casa", NULL, out, sizeof(out)) >= 0 &&
	          strcmp(out, "event disconnected enp8s0 casa") == 0,
	      "event disconnected");
	volatile size_t tiny = 10;
	CHECK(proto_event(0, "enp8s0", "casa", NULL, out, tiny) < 0,
	      "a line that does not fit is refused");
	CHECK(proto_connect_error(CONNECT_OK) == NULL &&
	          strcmp(proto_connect_error(CONNECT_NO_MATCH),
	                 "ERROR profile does not match") == 0,
	      "connect results become replies");
	return test_summary();
}
