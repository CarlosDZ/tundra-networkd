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
	struct iface_view v = {ci,       p,      conn, addr, unmanaged, kept,
	                       override, alerts, 0,    NULL, 0,         NULL};
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
	             "10.79.0.100/24 trusted no override - alert - ssid - bssid -"),
	      "a connected interface");
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state connecting profile casa address - "
	             "trusted no override - alert - ssid - bssid -"),
	      "a connection without lease");
	kept = "profile casa has errors";
	CHECK(status(&ci, &p, 1, "10.79.0.100/24", NULL,
	             "iface enp8s0 state connected profile casa address "
	             "10.79.0.100/24 trusted no override - alert - ssid - bssid - "
	             "-- previous "
	             "configuration kept: "
	             "profile casa has errors"),
	      "a connection kept by a reload says so");
	kept = NULL;
	ci.st = C_IDLE;
	CHECK(status(&ci, NULL, 0, NULL, NULL,
	             "iface enp8s0 state idle profile - address - trusted - "
	             "override - alert - ssid - bssid -"),
	      "an idle interface");
	CHECK(status(&ci, NULL, 0, NULL, "interfaces: line 2: unknown key speed",
	             "iface enp8s0 state unmanaged profile - address - trusted - "
	             "override - alert - ssid - bssid - "
	             "-- interfaces: line 2: unknown key speed"),
	      "an unmanaged interface carries its error");
	char out_off[512];
	struct iface_view off = {.ci = &ci, .off = 1};
	CHECK(proto_status(&off, out_off, sizeof(out_off)) >= 0 &&
	          strcmp(out_off,
	                 "iface enp8s0 state off profile - address - "
	                 "trusted - override - alert - ssid - bssid -") == 0,
	      "an interface with wifi = no is off");
	ci.st = C_BLOCKED;
	ci.blocked = B_CONFIG;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state blocked reason config profile casa "
	             "address - trusted no override - alert - ssid - bssid -"),
	      "a blocked interface carries its reason");
	ci.st = C_BACKOFF;
	CHECK(status(&ci, &p, 0, NULL, "x",
	             "iface enp8s0 state retrying profile casa address - "
	             "trusted no override - alert - ssid - bssid -"),
	      "retrying, and the unmanaged error only shows when idle");
	ci.st = C_WAIT_LINK;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state waiting profile casa address - "
	             "trusted no override - alert - ssid - bssid -"),
	      "waiting for the interface");
	ci.st = C_STOPPING;
	CHECK(status(&ci, &p, 0, NULL, NULL,
	             "iface enp8s0 state stopping profile casa address - "
	             "trusted no override - alert - ssid - bssid -"),
	      "stopping");

	ci.st = C_RUNNING;
	override = 7;
	alerts = "7,9";
	CHECK(
	    status(&ci, &p, 1, "10.79.0.100/24", NULL,
	           "iface enp8s0 state connected profile casa address "
	           "10.79.0.100/24 trusted no override 7 alert 7,9 ssid - bssid -"),
	    "an alert lowers trust and pending alerts are listed");
	p.trusted = 1;
	CHECK(
	    status(&ci, &p, 1, "10.79.0.100/24", NULL,
	           "iface enp8s0 state connected profile casa address "
	           "10.79.0.100/24 trusted no override 7 alert 7,9 ssid - bssid -"),
	    "even on a trusted profile");
	override = 0;
	alerts = NULL;
	CHECK(
	    status(&ci, &p, 1, "10.79.0.100/24", NULL,
	           "iface enp8s0 state connected profile casa address "
	           "10.79.0.100/24 trusted yes override - alert - ssid - bssid -"),
	    "a trusted profile without alerts");
	p.trusted = 0;

	char out[512];
	CHECK(proto_profile(&p, out, sizeof(out)) >= 0 &&
	          strcmp(out, "profile casa interface enp8s0 autoconnect yes "
	                      "trusted no ssid - security -") == 0,
	      "a profile line, without ssid nor security on a cable");
	struct profile w = {.name = "cafe",
	                    .interface = "wlp3s0",
	                    .ssid = "caf\xc3\xa9 1",
	                    .ssid_len = 7,
	                    .has_ssid = 1,
	                    .security = SEC_WPA3,
	                    .valid = 1};
	CHECK(proto_profile(&w, out, sizeof(out)) >= 0 &&
	          strcmp(out, "profile cafe interface wlp3s0 autoconnect no "
	                      "trusted no ssid caf%C3%A9%201 security wpa3") == 0,
	      "a WiFi profile with its ssid encoded and its security");
	w.security = SEC_OPEN;
	CHECK(proto_profile(&w, out, sizeof(out)) >= 0 &&
	          strstr(out, " security open"),
	      "an open one");
	struct iface_view assoc = {.ci = &ci,
	                           .p = &p,
	                           .connected = 1,
	                           .address = "10.80.0.119/24",
	                           .ssid = (const unsigned char *)"casa",
	                           .ssid_len = 4,
	                           .bssid = "02:00:00:00:00:00"};
	CHECK(proto_status(&assoc, out, sizeof(out)) >= 0 &&
	          strcmp(out, "iface enp8s0 state connected profile casa address "
	                      "10.80.0.119/24 trusted no override - alert - ssid "
	                      "casa bssid 02:00:00:00:00:00") == 0,
	      "an associated interface shows ssid and bssid at the end");
	char enc[8];
	CHECK(proto_encode((const unsigned char *)"-", 1, enc, sizeof(enc)) > 0 &&
	          strcmp(enc, "%2D") == 0,
	      "an ssid that is just a dash is encoded");
	CHECK(proto_encode((const unsigned char *)"a-b", 3, enc, sizeof(enc)) > 0 &&
	          strcmp(enc, "a-b") == 0,
	      "a dash inside is not");
	CHECK(proto_encode((const unsigned char *)"\0\x7f", 2, enc, sizeof(enc)) >
	              0 &&
	          strcmp(enc, "%00%7F") == 0,
	      "control bytes and NUL are encoded");
	CHECK(proto_encode((const unsigned char *)"abcd", 4, enc, 4) < 0,
	      "an encoding that does not fit is refused");
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
	CHECK(proto_alert_closed(7, "force", out, sizeof(out)) >= 0 &&
	          strcmp(out, "event alert-closed 7 force") == 0,
	      "event alert-closed");
	volatile size_t tiny = 10;
	CHECK(proto_event(0, "enp8s0", "casa", NULL, out, tiny) < 0,
	      "a line that does not fit is refused");
	CHECK(proto_connect_error(CONNECT_OK) == NULL &&
	          strcmp(proto_connect_error(CONNECT_NO_MATCH),
	                 "ERROR profile does not match") == 0,
	      "connect results become replies");
	CHECK(strcmp(proto_connect_error(CONNECT_OFF), "ERROR wifi is off") == 0,
	      "CONNECT on an interface with wifi = no");
	CHECK(parsed("NETWORKS", CMD_NETWORKS, 0) &&
	          parsed("NETWORKS wlp3s0", CMD_NETWORKS, 1) &&
	          parsed("SCAN", CMD_SCAN, 0) && parsed("SCAN wlp3s0", CMD_SCAN, 1),
	      "NETWORKS and SCAN, with or without interface");
	CHECK(bad("SCAN a b", "ERROR usage: SCAN [interface]"),
	      "SCAN with two arguments");
	struct wpa_bss bss = {.bssid = "02:00:00:00:01:00",
	                      .freq = 2412,
	                      .signal = -30,
	                      .sec = WSEC_WPA3,
	                      .ssid = "caf\xc3\xa9",
	                      .ssid_len = 5};
	CHECK(proto_network("wlp3s0", &bss, "casa", out, sizeof(out)) >= 0 &&
	          strcmp(out, "network wlp3s0 ssid caf%C3%A9 bssid "
	                      "02:00:00:00:01:00 freq 2412 signal -30 security "
	                      "wpa3 profile casa") == 0,
	      "a network line");
	bss.ssid_len = 0;
	CHECK(proto_network("wlp3s0", &bss, NULL, out, sizeof(out)) >= 0 &&
	          strstr(out, " ssid - ") && strstr(out, " profile -"),
	      "a hidden network without profile");
	bss.sec = WSEC_WPA2_WPA3;
	CHECK(proto_network("wlp3s0", &bss, NULL, out, sizeof(out)) >= 0 &&
	          strstr(out, " security wpa2/wpa3 "),
	      "transition mode is one token");
	CHECK(proto_scan("wlp3s0", 12, out, sizeof(out)) >= 0 &&
	          strcmp(out, "scan wlp3s0 age 12") == 0 &&
	          proto_scan("wlp3s0", -1, out, sizeof(out)) >= 0 &&
	          strcmp(out, "scan wlp3s0 age -") == 0,
	      "the age of the scan, or none yet");
	CHECK(proto_scan_event("wlp3s0", out, sizeof(out)) >= 0 &&
	          strcmp(out, "event scan wlp3s0") == 0,
	      "event scan");
	return test_summary();
}
