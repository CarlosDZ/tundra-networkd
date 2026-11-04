#include "conf/grammar.c"
#include "conf/ifaces.c"
#include "conf/profile.c"
#include "test.h"

static struct ifaces f;

static int parse(const char *s) { return ifaces_parse(&f, s, strlen(s)); }

static const struct iface_conf *get(const char *name) {
	return ifaces_find(&f, name);
}

int main(void) {
	parse("enp8s0 {\n metric = 100\n table = casa.cable\n}\n"
	      "wlp3s0 {\n metric = 600\n carrier-grace = 3000\n}\n");
	const struct iface_conf *c = get("enp8s0"), *w = get("wlp3s0");
	CHECK(f.valid && f.n == 2 && c && c->valid && c->has_metric &&
	          c->metric == 100 && c->has_table &&
	          strcmp(c->table, "casa.cable") == 0 && !c->has_grace,
	      "a cable block");
	CHECK(w && w->valid && w->metric == 600 && w->has_grace &&
	          w->grace_ms == 3000 && !w->has_table,
	      "a WiFi block");
	CHECK(get("eth9") == NULL, "an interface without block is not found");

	parse("enp8s0 {\n wifi = yes\n}\nwlp3s0 {\n metric = 1\n}\n");
	CHECK(f.valid && !get("enp8s0")->valid &&
	          strstr(get("enp8s0")->error, "unknown key wifi") &&
	          get("wlp3s0")->valid,
	      "wifi is an unknown key and only spoils its block");

	parse("enp8s0 {\n metric = 1\n metric = 2\n}\n");
	CHECK(!get("enp8s0")->valid &&
	          strstr(get("enp8s0")->error, "repeated key metric"),
	      "a repeated key");
	parse("enp8s0 {\n metric = abc\n}\n");
	CHECK(!get("enp8s0")->valid, "an invalid metric");
	parse("enp8s0 {\n metric = 4294967296\n}\n");
	CHECK(!get("enp8s0")->valid, "a metric beyond 32 bits");
	parse("enp8s0 {\n table = local\n}\n");
	CHECK(!get("enp8s0")->valid, "a reserved table");
	parse("enp8s0 {\n metric = \"100\"\n}\n");
	CHECK(!get("enp8s0")->valid &&
	          strstr(get("enp8s0")->error, "must not be quoted"),
	      "a quoted value");
	parse("enp8s0 {\n metric = 1\n}\nenp8s0 {\n metric = 2\n}\nwlp3s0 {\n}\n");
	CHECK(f.n == 2 && !get("enp8s0")->valid &&
	          strstr(get("enp8s0")->error, "repeated block") &&
	          get("wlp3s0")->valid,
	      "a repeated block spoils that interface only");

	parse("enp8s0 {\n metric = 1\n");
	CHECK(!f.valid && strstr(f.error, "not closed"),
	      "a syntax error spoils the whole file");
	parse("metric = 1\nenp8s0 {\n}\n");
	CHECK(!f.valid && strstr(f.error, "outside a block"),
	      "a key outside a block spoils the whole file");

	parse("");
	CHECK(f.valid && f.n == 0, "an empty file is valid");
	return test_summary();
}
