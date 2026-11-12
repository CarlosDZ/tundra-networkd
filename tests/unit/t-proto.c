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

static void test_profile_commands(void) {
	const char *usage =
	    "ERROR usage: PROFILE-SET <profile> <key> <value> [<key> <value>]...";
	CHECK(parsed("PROFILE casa", CMD_PROFILE, 1) &&
	          strcmp(c.args[0], "casa") == 0,
	      "PROFILE with a name");
	CHECK(bad("PROFILE", "ERROR usage: PROFILE <profile>") &&
	          bad("PROFILE a b", "ERROR usage: PROFILE <profile>"),
	      "PROFILE takes one name");
	CHECK(parsed("PROFILE-DELETE casa", CMD_PROFILE_DELETE, 1) &&
	          strcmp(c.args[0], "casa") == 0,
	      "PROFILE-DELETE with a name");
	CHECK(bad("PROFILE-DELETE", "ERROR usage: PROFILE-DELETE <profile>"),
	      "PROFILE-DELETE without a name");
	CHECK(parsed("PROFILE-SET casa  interface enp8s0 address dhcp",
	             CMD_PROFILE_SET, 1) &&
	          strcmp(c.args[0], "casa") == 0 &&
	          strcmp(c.rest, "  interface enp8s0 address dhcp") == 0,
	      "PROFILE-SET keeps the name and what follows it");
	CHECK(bad("PROFILE-SET", usage) && bad("PROFILE-SET casa", usage) &&
	          bad("PROFILE-SET casa interface", usage),
	      "PROFILE-SET needs a name and a pair");
	char line[1024] = "PROFILE-SET casa passphrase ";
	memset(line + strlen(line), 'a', 3 * PASSPHRASE_MAX);
	CHECK(parsed(line, CMD_PROFILE_SET, 1),
	      "a value of PROFILE-SET may be longer than a name");
	snprintf(line, sizeof(line), "PROFILE-SET %0*d interface enp8s0",
	         PROFILE_NAME_MAX + 1, 0);
	CHECK(bad(line, usage), "but its name may not");
}

static int decoded(const char *in, const char *want, size_t len) {
	char out[32];
	return proto_decode(in, strlen(in), out, sizeof(out)) == (int)len &&
	       memcmp(out, want, len) == 0 && out[len] == '\0';
}

static int undecoded(const char *in) {
	char out[32];
	return proto_decode(in, strlen(in), out, sizeof(out)) < 0;
}

static void test_decode(void) {
	CHECK(decoded("casa", "casa", 4) &&
	          decoded("a.b_c:d@e,f/g-h", "a.b_c:d@e,f/g-h", 15),
	      "safe bytes decode to themselves");
	CHECK(decoded("caf%C3%A9", "caf\xc3\xa9", 5) && decoded("%2D", "-", 1) &&
	          decoded("a%20b%0A", "a b\n", 4) &&
	          decoded("%c3%a9", "\xc3\xa9", 2),
	      "escapes decode to their byte, in either case");
	CHECK(undecoded("") && undecoded("a b") && undecoded("a*") &&
	          undecoded("caf\xc3\xa9") && undecoded("a\"b") && undecoded("a#"),
	      "a byte outside the safe set is refused");
	CHECK(undecoded("%") && undecoded("a%2") && undecoded("%zz") &&
	          undecoded("%2g") && undecoded("%%"),
	      "a broken escape is refused");
	CHECK(undecoded("a%00b"), "a NUL is refused");
	char out[4];
	CHECK(proto_decode("abc", 3, out, sizeof(out)) == 3 &&
	          proto_decode("abcd", 4, out, sizeof(out)) < 0 &&
	          proto_decode("a%41%41%41", 10, out, sizeof(out)) < 0,
	      "a value that does not fit is refused");
	unsigned char all[255];
	char enc[3 * sizeof(all) + 1], back[sizeof(all) + 1];
	for (size_t i = 0; i < sizeof(all); i++)
		all[i] = (unsigned char)(i + 1);
	int n = proto_encode(all, sizeof(all), enc, sizeof(enc));
	CHECK(n > 0 &&
	          proto_decode(enc, (size_t)n, back, sizeof(back)) ==
	              (int)sizeof(all) &&
	          memcmp(all, back, sizeof(all)) == 0,
	      "every byte but NUL goes through encode and decode");
}

static struct profile_draft dr;
static char derr[PROTO_ERR_MAX];

static int drafted(const char *words) {
	return proto_draft(words, &dr, derr, sizeof(derr)) == 0;
}

static int draft_fails(const char *words, const char *error) {
	return proto_draft(words, &dr, derr, sizeof(derr)) < 0 &&
	       strcmp(derr, error) == 0;
}

static int key_is(int k, const char *value) {
	return dr.key[k].named && dr.key[k].len == strlen(value) &&
	       strcmp(dr.key[k].value, value) == 0;
}

static void test_draft(void) {
	const char *usage =
	    "ERROR usage: PROFILE-SET <profile> <key> <value> [<key> <value>]...";
	CHECK(drafted(" interface enp8s0 address 192.168.1.50/24 gateway "
	              "192.168.1.1 dns 9.9.9.9,1.1.1.1 autoconnect yes\ttrusted no "
	              "mac 02:12:34:56:78:9a") &&
	          key_is(PKEY_INTERFACE, "enp8s0") &&
	          key_is(PKEY_ADDRESS, "192.168.1.50/24") &&
	          key_is(PKEY_GATEWAY, "192.168.1.1") &&
	          key_is(PKEY_DNS, "9.9.9.9,1.1.1.1") &&
	          key_is(PKEY_AUTOCONNECT, "yes") && key_is(PKEY_TRUSTED, "no") &&
	          key_is(PKEY_MAC, "02:12:34:56:78:9a") &&
	          !dr.key[PKEY_SSID].named && !dr.key[PKEY_PASSPHRASE].named,
	      "the pairs of a wired profile");
	CHECK(drafted("ssid caf%C3%A9 security wpa3 passphrase a%20%22b%5C") &&
	          key_is(PKEY_SSID, "caf\xc3\xa9") &&
	          key_is(PKEY_SECURITY, "wpa3") &&
	          key_is(PKEY_PASSPHRASE, "a \"b\\"),
	      "text values are decoded");
	CHECK(drafted("interface * ssid - gateway - passphrase %2D") &&
	          key_is(PKEY_INTERFACE, "*") && dr.key[PKEY_SSID].named &&
	          dr.key[PKEY_SSID].len == 0 && dr.key[PKEY_GATEWAY].named &&
	          dr.key[PKEY_GATEWAY].len == 0 && key_is(PKEY_PASSPHRASE, "-"),
	      "the wildcard, a dash for no value and an encoded dash");
	CHECK(draft_fails("", usage) && draft_fails("   ", usage),
	      "no pairs at all");
	CHECK(draft_fails("interface enp8s0 address", usage),
	      "a key without its value");
	CHECK(draft_fails("interface enp8s0 color red", usage) &&
	          draft_fails("Interface enp8s0", usage) &&
	          draft_fails("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa x", usage),
	      "an unknown key");
	CHECK(draft_fails("dns none dns lease", usage) &&
	          draft_fails("ssid - ssid casa", usage),
	      "a repeated key");
	CHECK(
	    draft_fails("ssid a*b", "ERROR invalid value for ssid") &&
	        draft_fails("ssid *", "ERROR invalid value for ssid") &&
	        draft_fails("address dhcp%00", "ERROR invalid value for address") &&
	        draft_fails("passphrase abc%4",
	                    "ERROR invalid value for passphrase"),
	    "a value with a bad encoding names its key");
	char words[640] = "passphrase ";
	for (int i = 0; i < PASSPHRASE_MAX; i++)
		strcat(words, "%FF");
	CHECK(drafted(words) && dr.key[PKEY_PASSPHRASE].len == PASSPHRASE_MAX,
	      "a passphrase of 128 encoded bytes");
	strcat(words, "a");
	CHECK(draft_fails(words, "ERROR invalid value for passphrase") &&
	          !strstr(derr, "%FF"),
	      "one byte more is refused, without the value in the answer");
}

static int detail(const struct profile *p, const char *want) {
	char out[512];
	return proto_profile_detail(p, out, sizeof(out)) >= 0 &&
	       strcmp(out, want) == 0;
}

static int detail_of(const char *words, const char *want) {
	struct profile p;
	char text[PROFILE_TEXT_MAX];
	return drafted(words) &&
	       profile_build(&p, "casa", &dr, text, sizeof(text)) > 0 &&
	       detail(&p, want);
}

static void test_detail(void) {
	CHECK(detail_of("interface enp8s0 address dhcp dns lease autoconnect yes "
	                "trusted no",
	                "profile casa interface enp8s0 ssid - security - "
	                "passphrase - address dhcp gateway - dns lease "
	                "autoconnect yes trusted no mac -"),
	      "the detail of a wired profile");
	CHECK(detail_of("interface * address 192.168.1.50/24 gateway 192.168.1.1 "
	                "dns 9.9.9.9,1.1.1.1,8.8.8.8 autoconnect no trusted yes "
	                "mac 02:12:34:56:78:9A",
	                "profile casa interface * ssid - security - passphrase - "
	                "address 192.168.1.50/24 gateway 192.168.1.1 dns "
	                "9.9.9.9,1.1.1.1,8.8.8.8 autoconnect no trusted yes mac "
	                "02:12:34:56:78:9a"),
	      "of a static one, with the wildcard and a fixed mac");
	CHECK(detail_of("interface wlp3s0 ssid caf%C3%A9%20%2D security wpa3 "
	                "passphrase top%20secret address dhcp dns none "
	                "autoconnect yes trusted no mac random",
	                "profile casa interface wlp3s0 ssid caf%C3%A9%20- security "
	                "wpa3 passphrase set address dhcp gateway - dns none "
	                "autoconnect yes trusted no mac random"),
	      "of a WiFi one: the passphrase is only set");
	CHECK(detail_of("interface wlp3s0 ssid - security - passphrase - address "
	                "dhcp gateway - dns lease autoconnect yes trusted no mac -",
	                "profile casa interface wlp3s0 ssid - security - "
	                "passphrase - address dhcp gateway - dns lease "
	                "autoconnect yes trusted no mac -"),
	      "the detail of a profile without passphrase is a valid request");
	CHECK(detail_of("interface wlp3s0 ssid cafe security open address dhcp "
	                "dns lease autoconnect yes trusted no mac keep",
	                "profile casa interface wlp3s0 ssid cafe security open "
	                "passphrase - address dhcp gateway - dns lease "
	                "autoconnect yes trusted no mac keep"),
	      "of an open network");
	struct profile broken = {.name = "roto", .error = "missing apply section"};
	CHECK(detail(&broken, "profile roto invalid -- missing apply section"),
	      "an invalid profile shows its error");
	char small[32];
	struct profile ok = {.name = "casa", .interface = "enp8s0", .valid = 1};
	CHECK(proto_profile_detail(&ok, small, sizeof(small)) < 0,
	      "a line that does not fit fails");
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
	test_profile_commands();
	test_decode();
	test_draft();
	test_detail();
	return test_summary();
}
