#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char dir[512];
static volatile sig_atomic_t got;

static void on_signal(int sig) { got = sig; }

static FILE *open_in(const char *name, const char *mode) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return fopen(path, mode);
}

static int read_mode(char *mode, size_t size) {
	const char *d = getenv("BIARP_FAKE_DIR");
	if (!d || !*d)
		return -1;
	snprintf(dir, sizeof(dir), "%s", d);
	FILE *f = open_in("mode", "r");
	if (!f)
		return -1;
	if (!fgets(mode, (int)size, f))
		mode[0] = '\0';
	fclose(f);
	mode[strcspn(mode, "\n")] = '\0';
	return 0;
}

static void set_mode(const char *mode) {
	FILE *f = open_in("mode", "w");
	if (f) {
		fprintf(f, "%s\n", mode);
		fclose(f);
	}
}

static const char *arg_after(int argc, char *argv[], const char *key) {
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], key) == 0)
			return argv[i + 1];
	return "-";
}

static int probe(int argc, char *argv[]) {
	char how[32] = "free";
	FILE *f = open_in("probe", "r");
	if (f) {
		if (!fgets(how, sizeof(how), f))
			snprintf(how, sizeof(how), "free");
		fclose(f);
		how[strcspn(how, "\n")] = '\0';
	}
	f = open_in("probes", "a");
	if (f) {
		for (int i = 1; i < argc; i++)
			fprintf(f, "%s%s", i > 1 ? " " : "", argv[i]);
		fputc('\n', f);
		fclose(f);
	}
	if (strcmp(how, "taken") == 0) {
		printf("taken mac 02:aa:aa:aa:aa:01 src 02:aa:aa:aa:aa:01\n");
		return 0;
	}
	if (strcmp(how, "alert") == 0) {
		fprintf(
		    stderr,
		    "alert 111/4 stop by tundra-blueice-arp on %s ts 0.000 reply "
		    "force,disconnect info case A mac 02:aa:aa:aa:aa:01 -- fake A\n",
		    argv[2]);
		return 111;
	}
	if (strncmp(how, "exit", 4) == 0)
		return atoi(how + 4);
	printf("free\n");
	return 0;
}

int main(int argc, char *argv[]) {
	char mode[32];
	if (read_mode(mode, sizeof(mode)) < 0) {
		const char *real = getenv("BIARP_REAL");
		execv(real && *real ? real : "/usr/local/bin/tundra-blueice-arp", argv);
		perror("execv error");
		return 1;
	}
	if (argc < 3)
		return 2;

	if (strcmp(argv[1], "probe") == 0)
		return probe(argc, argv);

	FILE *f = open_in("launches", "a");
	if (f) {
		fprintf(f, "%d", (int)getpid());
		for (int i = 1; i < argc; i++)
			fprintf(f, " %s", argv[i]);
		fputc('\n', f);
		fclose(f);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *ifname = argv[2], *addr = arg_after(argc, argv, "addr");

	if (strncmp(mode, "exit", 4) == 0) {
		printf("fake sensor exiting with %s\n", mode + 4);
		return atoi(mode + 4);
	}
	if (strcmp(mode, "conflict") == 0) {
		set_mode("run");
		printf("conflict addr %s mac 02:aa:aa:aa:aa:01\n", addr);
		return 1;
	}
	char verb[16], kase[32];
	int level = 0;
	if (sscanf(mode, "%15[^:]:%31[^:]:%d", verb, kase, &level) == 3) {
		char want[48];
		const char *acc = arg_after(argc, argv, "accept");
		int covered = 0;
		for (int l = level; l <= 10 && !covered; l++) {
			snprintf(want, sizeof(want), "%s/%d", kase, l);
			const char *hit = strstr(acc, want);
			size_t wl = strlen(want);
			covered = hit && (hit == acc || hit[-1] == ',') &&
			          (hit[wl] == '\0' || hit[wl] == ',');
		}
		int stop = strcmp(verb, "stop") == 0 && !covered;
		set_mode("run");
		fprintf(stderr,
		        "alert 111/%d %s by tundra-blueice-arp on %s ts 0.000 reply %s "
		        "info case %s gw %s -- fake %s\n",
		        level, stop ? "stop" : "warn", ifname,
		        stop ? "force,disconnect" : "ack,disconnect", kase,
		        arg_after(argc, argv, "gw"), kase);
		if (stop)
			return 111;
	}
	if (strcmp(mode, "forge") == 0) {
		set_mode("run");
		fprintf(stderr,
		        "alert 111/8 stop by tundra-dhcpc on %s ts 0.000 reply "
		        "force,disconnect info case x -- forged\n",
		        ifname);
	}

	struct sigaction sa = {.sa_handler = on_signal};
	sigaction(SIGTERM, &sa, NULL);
	printf("fake sensor watching %s\n", addr);
	struct timespec tick = {0, 100000000};
	while (!got)
		nanosleep(&tick, NULL);
	f = open_in("signals", "a");
	if (f) {
		fprintf(f, "%d TERM\n", (int)getpid());
		fclose(f);
	}
	return 0;
}
