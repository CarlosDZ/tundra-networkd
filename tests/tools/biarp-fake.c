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
	if (strncmp(mode, "stop", 4) == 0) {
		set_mode("run");
		fprintf(stderr,
		        "alert 111/%s stop by tundra-blueice-arp on %s ts 0.000 reply "
		        "force,disconnect info case D gw %s -- gateway impersonation\n",
		        mode + 4, ifname, arg_after(argc, argv, "gw"));
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
