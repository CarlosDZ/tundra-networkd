#include "conf/profile.c"
#include "test.h"

static struct profile p;

#define WIFI_MATCH                                                             \
	"match {\n interface = wlp3s0\n ssid = \"casa\"\n security = wpa3\n}\n"
#define WIFI_APPLY                                                             \
	"apply {\n passphrase = \"correcthorsebatterystaple\"\n address = dhcp\n"  \
	" dns = lease\n autoconnect = yes\n trusted = yes\n}\n"
#define WIRED_MATCH "match {\n interface = enp8s0\n}\n"

static int tnc(const char *s) {
	return profile_parse(&p, "10-casa", s, strlen(s), 0);
}

static int wired(const char *apply) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%sapply {\n%s}\n", WIRED_MATCH, apply);
	return tnc(buf);
}

static int wifi(const char *match, const char *apply) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "match {\n%s}\napply {\n%s}\n", match, apply);
	return tnc(buf);
}

static int has(const char *text) { return strstr(p.error, text) != NULL; }

#define WIRED_REST " dns = none\n autoconnect = no\n trusted = no\n"

static void test_valid(void) {
	CHECK(tnc(WIFI_MATCH WIFI_APPLY) == 0 && p.valid &&
	          strcmp(p.interface, "wlp3s0") == 0 && p.has_ssid &&
	          p.ssid_len == 4 && memcmp(p.ssid, "casa", 4) == 0 &&
	          p.security == SEC_WPA3 && p.has_passphrase &&
	          p.address == ADDR_DHCP && p.dns == DNS_LEASE && p.autoconnect &&
	          p.trusted && strcmp(p.name, "10-casa") == 0,
	      "a WiFi profile");
	CHECK(
	    wired(" address = 192.168.1.50/24\n gateway = 192.168.1.1\n"
	          " dns = 1.1.1.1,  9.9.9.9\n autoconnect = no\n trusted = no\n") ==
	            0 &&
	        p.address == ADDR_STATIC && p.ip == inet_addr("192.168.1.50") &&
	        p.prefix == 24 && p.has_gateway &&
	        p.gateway == inet_addr("192.168.1.1") && p.dns == DNS_LIST &&
	        p.ndns == 2 && p.dns_list[1] == inet_addr("9.9.9.9") &&
	        !p.autoconnect && !p.trusted && !p.has_ssid,
	    "a wired profile with a static address");
	CHECK(tnc("match {\n interface = *\n}\napply {\n address = dhcp\n"
	          " dns = lease\n autoconnect = yes\n trusted = no\n}\n") == 0 &&
	          p.any_interface,
	      "the 999-dhcp.tnc profile");
	CHECK(wired(" address = 10.0.0.0/31\n" WIRED_REST) == 0,
	      "a /31 has no network or broadcast address");
	CHECK(wired(" address = 10.0.0.7/32\n" WIRED_REST) == 0, "a /32 address");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"cafe\"\n security = open\n",
	           " address = dhcp\n dns = lease\n autoconnect = no\n"
	           " trusted = no\n") == 0 &&
	          p.security == SEC_OPEN && !p.has_passphrase,
	      "an open network without passphrase");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"x\"\n security = wpa3\n",
	           " passphrase = \"k\"\n address = dhcp\n dns = lease\n"
	           " autoconnect = no\n trusted = no\n") == 0,
	      "a one-character wpa3 password");
}

static void test_structure(void) {
	CHECK(
	    tnc("match {\n interface = wlp3s0\n ssid = \"casa\"\n securty = wpa3\n}"
	        "\n" WIFI_APPLY) < 0 &&
	        has("unknown key securty"),
	    "an unknown key");
	CHECK(tnc("match {\n interface = wlp3s0\n ssid = \"a\"\n ssid = \"b\"\n"
	          " security = wpa3\n}\n" WIFI_APPLY) < 0 &&
	          has("repeated key ssid"),
	      "a repeated key");
	CHECK(
	    tnc("match {\n interface = wlp3s0\n ssid = \"casa\"\n security = wpa3\n"
	        " passphrase = \"x\"\n}\n" WIFI_APPLY) < 0 &&
	        has("passphrase belongs in apply"),
	    "a key in the wrong section");
	CHECK(tnc("ssid = \"casa\"\n" WIFI_MATCH WIFI_APPLY) < 0 &&
	          has("outside a section"),
	      "a key outside any section");
	CHECK(tnc("matc {\n}\n" WIFI_MATCH WIFI_APPLY) < 0 &&
	          has("unknown section matc"),
	      "an unknown section");
	CHECK(tnc(WIFI_MATCH) < 0 && has("missing apply section"),
	      "a missing apply section");
	CHECK(tnc(WIFI_APPLY) < 0 && has("missing match section"),
	      "a missing match section");
	CHECK(tnc(WIFI_MATCH WIFI_MATCH WIFI_APPLY) < 0 &&
	          has("repeated section match"),
	      "a repeated section");
	CHECK(tnc("match {\n interface = x\n") < 0 && has("not closed"),
	      "a syntax error");
}

static void test_required(void) {
	CHECK(tnc("match {\n}\napply {\n address = dhcp\n dns = lease\n"
	          " autoconnect = no\n trusted = no\n}\n") < 0 &&
	          has("missing interface"),
	      "missing interface");
	CHECK(wired(" dns = none\n autoconnect = no\n trusted = no\n") < 0 &&
	          has("missing address"),
	      "missing address");
	CHECK(wired(" address = dhcp\n autoconnect = no\n trusted = no\n") < 0 &&
	          has("missing dns"),
	      "missing dns");
	CHECK(wired(" address = dhcp\n dns = none\n trusted = no\n") < 0 &&
	          has("missing autoconnect"),
	      "missing autoconnect");
	CHECK(wired(" address = dhcp\n dns = none\n autoconnect = no\n") < 0 &&
	          has("missing trusted"),
	      "missing trusted");
}

static void test_quoting(void) {
	CHECK(wifi(" interface = wlp3s0\n ssid = Caf\xc3\xa9 Libre #2\n"
	           " security = open\n",
	           " address = dhcp\n dns = lease\n autoconnect = no\n"
	           " trusted = no\n") < 0 &&
	          has("ssid must be quoted"),
	      "an unquoted ssid is not cut silently");
	CHECK(wired(" address = \"dhcp\"\n" WIRED_REST) < 0 &&
	          has("address must not be quoted"),
	      "a quoted token");
}

static void test_wifi(void) {
	const char *apply_dhcp =
	    " address = dhcp\n dns = lease\n autoconnect = no\n trusted = no\n";
	CHECK(wired(" passphrase = \"x\"\n address = dhcp\n" WIRED_REST) < 0,
	      "a passphrase without ssid");
	CHECK(tnc("match {\n interface = enp8s0\n security = open\n}\napply {\n"
	          " address = dhcp\n" WIRED_REST "}\n") < 0 &&
	          has("security requires ssid"),
	      "security without ssid");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"casa\"\n", apply_dhcp) < 0 &&
	          has("missing security"),
	      "an ssid without security");
	CHECK(
	    wifi(" interface = wlp3s0\n ssid = \"casa\"\n security = transition\n",
	         apply_dhcp) < 0 &&
	        has("invalid security"),
	    "there is no transition mode");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"casa\"\n security = wpa2\n",
	           apply_dhcp) < 0 &&
	          has("missing passphrase"),
	      "wpa2 without passphrase");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"casa\"\n security = open\n",
	           " passphrase = \"12345678\"\n address = dhcp\n dns = lease\n"
	           " autoconnect = no\n trusted = no\n") < 0,
	      "a passphrase on an open network");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"casa\"\n security = wpa2\n",
	           " passphrase = \"1234567\"\n address = dhcp\n dns = lease\n"
	           " autoconnect = no\n trusted = no\n") < 0,
	      "a wpa2 passphrase of 7 characters");
	CHECK(
	    wifi(
	        " interface = wlp3s0\n ssid = \"casa\"\n security = wpa2\n",
	        " passphrase = "
	        "\"1234567890123456789012345678901234567890123456789012345678901234"
	        "\"\n address = dhcp\n dns = lease\n autoconnect = no\n"
	        " trusted = no\n") < 0,
	    "a wpa2 passphrase of 64 characters");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"casa\"\n security = wpa2\n",
	           " passphrase = \"1234567\\x01\"\n address = dhcp\n dns = lease\n"
	           " autoconnect = no\n trusted = no\n") < 0 &&
	          has("printable"),
	      "a wpa2 passphrase with a control byte");
	CHECK(wifi(" interface = wlp3s0\n ssid = \"\"\n security = open\n",
	           apply_dhcp) < 0,
	      "an empty ssid");
	CHECK(
	    wifi(
	        " interface = wlp3s0\n ssid = \"123456789012345678901234567890123\""
	        "\n security = open\n",
	        apply_dhcp) < 0,
	    "an ssid of 33 bytes");
}

static void test_values(void) {
	CHECK(wired(" address = 192.168.1.50\n" WIRED_REST) < 0 &&
	          has("needs a prefix"),
	      "an address without prefix");
	CHECK(wired(" address = 192.168.1.50/0\n" WIRED_REST) < 0, "a prefix of 0");
	CHECK(wired(" address = 192.168.1.50/33\n" WIRED_REST) < 0,
	      "a prefix of 33");
	CHECK(wired(" address = 192.168.1.0/24\n" WIRED_REST) < 0 &&
	          has("network or broadcast"),
	      "the network address");
	CHECK(wired(" address = 192.168.1.255/24\n" WIRED_REST) < 0 &&
	          has("network or broadcast"),
	      "the broadcast address");
	CHECK(wired(" address = dhcp\n gateway = 192.168.1.1\n" WIRED_REST) < 0 &&
	          has("requires a static address"),
	      "a gateway with dhcp");
	CHECK(
	    wired(" address = 192.168.1.50/24\n gateway = 10.0.0.1\n" WIRED_REST) <
	            0 &&
	        has("outside"),
	    "a gateway outside the network");
	CHECK(wired(" address = 192.168.1.50/24\n gateway = "
	            "192.168.1.50\n" WIRED_REST) < 0,
	      "the gateway is the address itself");
	CHECK(wired(" address = 192.168.1.50/24\n dns = lease\n autoconnect = no\n"
	            " trusted = no\n") < 0 &&
	          has("requires address = dhcp"),
	      "dns = lease with a static address");
	CHECK(wired(" address = dhcp\n dns = 1.1.1.1, 2.2.2.2, 3.3.3.3, 4.4.4.4\n"
	            " autoconnect = no\n trusted = no\n") < 0 &&
	          has("more than 3"),
	      "four dns servers");
	CHECK(wired(" address = dhcp\n dns = 1.1.1.1 ,2.2.2.2\n autoconnect = no\n"
	            " trusted = no\n") < 0,
	      "a space before the comma");
	CHECK(wired(" address = dhcp\n dns = none\n autoconnect = maybe\n"
	            " trusted = no\n") < 0 &&
	          has("yes or no"),
	      "autoconnect must be yes or no");
	CHECK(tnc("match {\n interface = a/b\n}\napply {\n address = "
	          "dhcp\n" WIRED_REST "}\n") < 0 &&
	          has("invalid interface"),
	      "an invalid interface name");
}

static void test_mac(void) {
	static const unsigned char fixed[MAC_LEN] = {0x02, 0x12, 0x34,
	                                             0x56, 0x78, 0x9a};
	CHECK(wired(" address = dhcp\n" WIRED_REST) == 0 && p.mac.mode == MAC_UNSET,
	      "without mac the profile follows the interface");
	CHECK(wired(" address = dhcp\n mac = keep\n" WIRED_REST) == 0 &&
	          p.mac.mode == MAC_KEEP,
	      "mac = keep");
	CHECK(wired(" address = dhcp\n mac = random\n" WIRED_REST) == 0 &&
	          p.mac.mode == MAC_RANDOM,
	      "mac = random");
	CHECK(
	    wired(" address = dhcp\n mac = 02:12:34:56:78:9A\n" WIRED_REST) == 0 &&
	        p.mac.mode == MAC_FIXED && memcmp(p.mac.addr, fixed, MAC_LEN) == 0,
	    "a fixed mac");
	CHECK(wired(" address = dhcp\n mac = 01:12:34:56:78:9a\n" WIRED_REST) < 0 &&
	          has("invalid mac"),
	      "a multicast mac");
	CHECK(wired(" address = dhcp\n mac = 00:00:00:00:00:00\n" WIRED_REST) < 0,
	      "the zero mac");
	CHECK(wired(" address = dhcp\n mac = 02:12:34:56:78\n" WIRED_REST) < 0 &&
	          wired(" address = dhcp\n mac = 02-12-34-56-78-9a\n" WIRED_REST) <
	              0 &&
	          wired(" address = dhcp\n mac = 02:12:34:56:78:9g\n" WIRED_REST) <
	              0 &&
	          wired(" address = dhcp\n mac = stable\n" WIRED_REST) < 0,
	      "a short one, other separators, a bad digit and an unknown word");
	CHECK(wired(" address = dhcp\n mac = \"random\"\n" WIRED_REST) < 0,
	      "a quoted mac");
}

static void test_exposure(void) {
	CHECK(profile_parse(&p, "10-casa", WIFI_MATCH WIFI_APPLY,
	                    strlen(WIFI_MATCH WIFI_APPLY), 1) < 0 &&
	          has("readable by another group or by others"),
	      "credentials in an exposed file");
	const char *open =
	    WIRED_MATCH "apply {\n address = dhcp\n" WIRED_REST "}\n";
	CHECK(profile_parse(&p, "20-cable", open, strlen(open), 1) == 0,
	      "an exposed file without credentials is fine");
}

static void test_names(void) {
	char name[PROFILE_NAME_MAX + 1];
	CHECK(profile_name_from_file("10-casa.tnc", name) == 0 &&
	          strcmp(name, "10-casa") == 0,
	      "the profile name is the file name without .tnc");
	CHECK(profile_name_from_file("notes.txt", name) == -1, "not a profile");
	CHECK(profile_name_from_file(".tnc", name) == -1, "no name");
	CHECK(profile_name_from_file(".hidden.tnc", name) == -2,
	      "a hidden file is not a valid name");
	CHECK(profile_name_from_file("bad name.tnc", name) == -2,
	      "spaces are not valid in a name");
	CHECK(!conf_valid_ifname("") && !conf_valid_ifname(".") &&
	          !conf_valid_ifname("0123456789abcdef") &&
	          conf_valid_ifname("eth0"),
	      "interface names follow the kernel rules");
}

static struct profile_draft d;
static char text[PROFILE_TEXT_MAX];

static void draft(int k, const char *value) {
	d.key[k].named = 1;
	d.key[k].len = strlen(value);
	memcpy(d.key[k].value, value, d.key[k].len + 1);
}

static void wired_draft(void) {
	memset(&d, 0, sizeof(d));
	draft(PKEY_INTERFACE, "enp8s0");
	draft(PKEY_ADDRESS, "dhcp");
	draft(PKEY_DNS, "lease");
	draft(PKEY_AUTOCONNECT, "yes");
	draft(PKEY_TRUSTED, "no");
}

static void wifi_draft(const char *pass) {
	wired_draft();
	draft(PKEY_SSID, "casa");
	draft(PKEY_SECURITY, "wpa3");
	if (pass)
		draft(PKEY_PASSPHRASE, pass);
}

static int build(void) {
	return profile_build(&p, "10-casa", &d, text, sizeof(text));
}

static void test_build(void) {
	struct profile again;

	wired_draft();
	CHECK(build() > 0 && p.valid && strcmp(p.name, "10-casa") == 0 &&
	          strcmp(p.interface, "enp8s0") == 0 && p.address == ADDR_DHCP &&
	          p.dns == DNS_LEASE && p.autoconnect && !p.trusted &&
	          !p.has_ssid && p.mac.mode == MAC_UNSET,
	      "a wired draft builds a valid profile");
	CHECK(strcmp(text, "match {\n\tinterface = enp8s0\n}\n\napply {\n"
	                   "\taddress = dhcp\n\tdns = lease\n"
	                   "\tautoconnect = yes\n\ttrusted = no\n}\n") == 0,
	      "with the text of a profile written by hand");
	CHECK(profile_parse(&again, "10-casa", text, strlen(text), 0) == 0 &&
	          again.valid,
	      "that loads as any other file");

	wired_draft();
	draft(PKEY_INTERFACE, "*");
	draft(PKEY_ADDRESS, "192.168.1.50/24");
	draft(PKEY_GATEWAY, "192.168.1.1");
	draft(PKEY_DNS, "9.9.9.9,1.1.1.1");
	draft(PKEY_MAC, "02:12:34:56:78:9a");
	CHECK(build() > 0 && p.any_interface && p.address == ADDR_STATIC &&
	          p.prefix == 24 && p.has_gateway && p.ndns == 2 &&
	          p.mac.mode == MAC_FIXED,
	      "a static draft with gateway, dns list and mac");

	wifi_draft("correct horse");
	CHECK(build() > 0 && p.has_ssid && p.security == SEC_WPA3 &&
	          strcmp(p.passphrase, "correct horse") == 0,
	      "a WiFi draft");

	wifi_draft("a\"b\\c #d\n\x01\x7f\xc3\xa9}");
	d.key[PKEY_SSID].len = 6;
	memcpy(d.key[PKEY_SSID].value, "\"x\\\n#\xff", 6);
	CHECK(build() > 0 && p.ssid_len == 6 &&
	          memcmp(p.ssid, "\"x\\\n#\xff", 6) == 0 &&
	          strcmp(p.passphrase, "a\"b\\c #d\n\x01\x7f\xc3\xa9}") == 0,
	      "quotes, backslashes, newlines and any byte survive the round trip");
	CHECK(strstr(text, "\tssid = \"\\\"x\\\\\\x0a#\xff\"\n") &&
	          strstr(text, "\tpassphrase = \"a\\\"b\\\\c #d\\x0a\\x01\\x7f"
	                       "\xc3\xa9}\"\n"),
	      "escaped as the grammar asks");

	char longest[PASSPHRASE_MAX + 1];
	memset(longest, 0xc3, PASSPHRASE_MAX);
	longest[PASSPHRASE_MAX] = '\0';
	wifi_draft(longest);
	CHECK(build() > 0 && strcmp(p.passphrase, longest) == 0,
	      "a passphrase of 128 bytes outside ASCII fits");
	memset(longest, '"', PASSPHRASE_MAX);
	wifi_draft(longest);
	CHECK(build() > 0 && strcmp(p.passphrase, longest) == 0,
	      "and one of 128 quotes");

	wired_draft();
	draft(PKEY_ADDRESS, "dhcp\n\ttrusted = yes");
	CHECK(build() < 0 && !p.valid && has("invalid value for address"),
	      "a bare value cannot carry another line");
	wired_draft();
	draft(PKEY_DNS, "none #");
	CHECK(build() < 0 && has("invalid value for dns"),
	      "nor a comment or a space");
	wired_draft();
	draft(PKEY_INTERFACE, "en\"p");
	CHECK(build() < 0 && has("invalid value for interface"), "nor a quote");

	wired_draft();
	draft(PKEY_ADDRESS, "10.0.0.1");
	CHECK(build() < 0 && !p.valid &&
	          strcmp(p.error, "address needs a prefix: 10.0.0.1") == 0,
	      "the error of the parser comes without the line");
	wired_draft();
	memset(&d.key[PKEY_TRUSTED], 0, sizeof(d.key[0]));
	d.key[PKEY_TRUSTED].named = 1;
	CHECK(build() < 0 && strcmp(p.error, "missing trusted") == 0,
	      "a key without value is a missing key");
	wifi_draft(NULL);
	CHECK(build() < 0 && strcmp(p.error, "missing passphrase") == 0,
	      "a WiFi draft without passphrase is refused");
	wifi_draft("short");
	draft(PKEY_SECURITY, "wpa2");
	CHECK(build() < 0 && has("wpa2 passphrase must have 8 to 63") &&
	          !strstr(p.error, "short"),
	      "a bad passphrase is refused without being quoted");
	wired_draft();
	char small[16];
	CHECK(profile_build(&p, "10-casa", &d, small, sizeof(small)) < 0 &&
	          has("profile too long"),
	      "a text that does not fit is refused");
}

static void test_keep(void) {
	struct profile old;
	const char *tnc_old = WIFI_MATCH WIFI_APPLY;
	profile_parse(&old, "10-casa", tnc_old, strlen(tnc_old), 0);

	wifi_draft(NULL);
	profile_draft_keep(&d, &old);
	CHECK(build() > 0 && strcmp(p.passphrase, "correcthorsebatterystaple") == 0,
	      "a draft without passphrase keeps the stored one");
	wifi_draft("another one");
	profile_draft_keep(&d, &old);
	CHECK(build() > 0 && strcmp(p.passphrase, "another one") == 0,
	      "one with passphrase replaces it");
	wifi_draft(NULL);
	d.key[PKEY_PASSPHRASE].named = 1;
	profile_draft_keep(&d, &old);
	CHECK(build() < 0 && has("missing passphrase"),
	      "a passphrase named without value drops the stored one");
	wifi_draft(NULL);
	draft(PKEY_SECURITY, "open");
	profile_draft_keep(&d, &old);
	CHECK(build() > 0 && !p.has_passphrase,
	      "a network that turns open does not keep it");
	wired_draft();
	profile_draft_keep(&d, &old);
	CHECK(build() > 0 && !p.has_passphrase, "nor a wired one");
	wifi_draft(NULL);
	draft(PKEY_SSID, "otra");
	profile_draft_keep(&d, &old);
	CHECK(build() < 0 && has("missing passphrase"),
	      "nor one that points to another network");
	wifi_draft(NULL);
	profile_draft_keep(&d, NULL);
	CHECK(build() < 0 && has("missing passphrase"),
	      "a new profile has nothing to keep");
	old.valid = 0;
	wifi_draft(NULL);
	profile_draft_keep(&d, &old);
	CHECK(build() < 0, "nor one whose stored profile has errors");
}

static void test_hostile_names(void) {
	char longname[PROFILE_NAME_MAX + 2];
	memset(longname, 'a', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	CHECK(profile_valid_name("10-casa") && profile_valid_name("a.b_c-D9"),
	      "a valid profile name");
	CHECK(!profile_valid_name("") && !profile_valid_name(".") &&
	          !profile_valid_name("..") && !profile_valid_name(".hidden"),
	      "empty, dot and hidden names are refused");
	CHECK(!profile_valid_name("../casa") && !profile_valid_name("a/b") &&
	          !profile_valid_name("/etc/passwd") &&
	          !profile_valid_name("a b") && !profile_valid_name("a\nb") &&
	          !profile_valid_name("caf\xc3\xa9"),
	      "slashes, spaces, newlines and bytes outside ASCII are refused");
	CHECK(!profile_valid_name(longname), "a name over the limit is refused");
	longname[PROFILE_NAME_MAX] = '\0';
	CHECK(profile_valid_name(longname), "one at the limit is valid");
	CHECK(profile_key("ssid") == PKEY_SSID && profile_key("mac") == PKEY_MAC &&
	          profile_key("nope") < 0 && profile_key("") < 0,
	      "keys are found by name");
}

int main(void) {
	test_build();
	test_keep();
	test_hostile_names();
	test_valid();
	test_structure();
	test_required();
	test_quoting();
	test_wifi();
	test_values();
	test_mac();
	test_exposure();
	test_names();
	return test_summary();
}
