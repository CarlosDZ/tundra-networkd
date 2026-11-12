#include "conf/ifaces.c"
#include "conf/load.c"
#include "conf/profile.c"
#include "test.h"

#include <sys/stat.h>

static char dir[] = "/tmp/tnd-conf.XXXXXX";
static struct config c;

static const char *wired =
    "match {\n interface = enp8s0\n}\napply {\n address = dhcp\n dns = lease\n"
    " autoconnect = yes\n trusted = no\n}\n";
static const char *wifi =
    "match {\n interface = wlp3s0\n ssid = \"casa\"\n security = wpa2\n}\n"
    "apply {\n passphrase = \"12345678\"\n address = dhcp\n dns = lease\n"
    " autoconnect = yes\n trusted = yes\n}\n";

static void put(const char *file, const char *text, mode_t mode) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
	chmod(path, mode);
}

static void drop(const char *file) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	unlink(path);
}

int main(void) {
	if (!freopen("/dev/null", "w", stderr) || !mkdtemp(dir))
		return 1;

	put("20-b.tnc", wired, 0644);
	put("10-a.tnc", wired, 0644);
	put("000-template.tnc", "# template\n", 0644);
	put("notes.txt", "not a profile", 0644);
	put("30-c.tnc", "garbage\n", 0644);
	put("15-wifi.tnc", wifi, 0600);

	CHECK(conf_load(dir, &c) == 0 && c.n == 4,
	      "loads every .tnc but the template");
	CHECK(strcmp(c.profiles[0].name, "10-a") == 0 &&
	          strcmp(c.profiles[1].name, "15-wifi") == 0 &&
	          strcmp(c.profiles[2].name, "20-b") == 0 &&
	          strcmp(c.profiles[3].name, "30-c") == 0,
	      "profiles in file name order");
	CHECK(c.profiles[0].valid && c.profiles[1].valid && c.profiles[2].valid &&
	          !c.profiles[3].valid,
	      "an invalid profile does not affect the others");
	CHECK(c.ifaces.valid && c.ifaces.n == 0,
	      "a missing interfaces file is an empty one");

	chmod(dir, 0700);
	put("15-wifi.tnc", wifi, 0640);
	conf_load(dir, &c);
	CHECK(c.profiles[1].valid,
	      "credentials readable by the group of the daemon are accepted");
	put("15-wifi.tnc", wifi, 0604);
	conf_load(dir, &c);
	CHECK(!c.profiles[1].valid && strstr(c.profiles[1].error, "readable"),
	      "credentials readable by others are rejected");
	struct stat st = {.st_mode = S_IFREG | 0600, .st_gid = 7};
	CHECK(!exposed(&st, 7) && !exposed(&st, 8), "0600 is not exposed");
	st.st_mode = S_IFREG | 0640;
	CHECK(!exposed(&st, 7) && exposed(&st, 8),
	      "0640 is exposed unless the group is the one of the daemon");
	st.st_mode = S_IFREG | 0660;
	CHECK(!exposed(&st, 7) && exposed(&st, 8), "and so is 0660");
	st.st_mode = S_IFREG | 0620;
	CHECK(exposed(&st, 8), "a file another group can write is exposed");
	st.st_mode = S_IFREG | 0601;
	CHECK(exposed(&st, 7), "any access of others is exposed");

	put("interfaces", "enp8s0 {\n metric = 100\n}\n", 0644);
	conf_load(dir, &c);
	CHECK(c.ifaces.valid && c.ifaces.n == 1 && c.ifaces.items[0].metric == 100,
	      "the interfaces file is read");

	char target[256], link[256];
	snprintf(target, sizeof(target), "%s/10-a.tnc", dir);
	snprintf(link, sizeof(link), "%s/40-link.tnc", dir);
	CHECK(symlink(target, link) == 0, "a symlinked profile is created");
	conf_load(dir, &c);
	CHECK(c.n == 5 && !c.profiles[4].valid, "a symlinked profile is rejected");
	unlink(link);

	for (int i = 0; i < PROFILES_MAX + 2; i++) {
		char name[32];
		snprintf(name, sizeof(name), "%03d-p.tnc", 100 + i);
		put(name, wired, 0644);
	}
	conf_load(dir, &c);
	CHECK(c.n == PROFILES_MAX && c.truncated &&
	          strcmp(c.profiles[0].name, "10-a") == 0,
	      "beyond the limit the first ones in order are kept");
	for (int i = 0; i < PROFILES_MAX + 2; i++) {
		char name[32];
		snprintf(name, sizeof(name), "%03d-p.tnc", 100 + i);
		drop(name);
	}

	CHECK(conf_load("/nonexistent/tnd", &c) < 0, "a missing directory fails");

	drop("20-b.tnc");
	drop("10-a.tnc");
	drop("000-template.tnc");
	drop("notes.txt");
	drop("30-c.tnc");
	drop("15-wifi.tnc");
	drop("interfaces");
	rmdir(dir);
	return test_summary();
}
