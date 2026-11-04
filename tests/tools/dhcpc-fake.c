#include <dirent.h>
#include <net/if.h>
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

static void record_sigs(void) {
	FILE *in = fopen("/proc/self/status", "r");
	FILE *out = open_in("sigs", "w");
	char line[256];
	while (in && out && fgets(line, sizeof(line), in))
		if (strncmp(line, "SigBlk:", 7) == 0 ||
		    strncmp(line, "SigIgn:", 7) == 0)
			fputs(line, out);
	if (in)
		fclose(in);
	if (out)
		fclose(out);
}

static void record_fds(void) {
	DIR *d = opendir("/proc/self/fd");
	FILE *out = open_in("fds", "w");
	struct dirent *e;
	while (d && out && (e = readdir(d)))
		if (e->d_name[0] != '.' && atoi(e->d_name) != dirfd(d) &&
		    atoi(e->d_name) != fileno(out))
			fprintf(out, "%s\n", e->d_name);
	if (d)
		closedir(d);
	if (out)
		fclose(out);
}

static void record_launch(int argc, char *argv[]) {
	FILE *f = open_in("launches", "a");
	if (!f)
		return;
	fprintf(f, "%d", (int)getpid());
	for (int i = 1; i < argc; i++)
		fprintf(f, " %s", argv[i]);
	fputc('\n', f);
	fclose(f);
}

static int read_mode(char *mode, size_t size) {
	const char *d = getenv("DHCPC_FAKE_DIR");
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

int main(int argc, char *argv[]) {
	char mode[32];
	if (read_mode(mode, sizeof(mode)) < 0) {
		const char *real = getenv("DHCPC_REAL");
		execv(real && *real ? real : "/usr/local/bin/tundra-dhcpc", argv);
		perror("execv error");
		return 1;
	}
	if (argc < 2)
		return 2;

	record_sigs();
	record_fds();
	record_launch(argc, argv);
	if (strncmp(mode, "exit", 4) == 0) {
		printf("fake exiting with %s\n", mode + 4);
		return atoi(mode + 4);
	}

	struct sigaction sa = {.sa_handler = on_signal};
	if (strcmp(mode, "stubborn") == 0)
		sa.sa_handler = SIG_IGN;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	printf("fake running\n");
	fflush(stdout);

	struct timespec tick = {0, 100000000};
	while (!got && if_nametoindex(argv[1]))
		nanosleep(&tick, NULL);
	if (got) {
		FILE *f = open_in("signals", "a");
		if (f) {
			fprintf(f, "%d %s\n", (int)getpid(),
			        got == SIGTERM ? "TERM" : "USR1");
			fclose(f);
		}
		return 0;
	}
	printf("interface %s removed\n", argv[1]);
	return 1;
}
