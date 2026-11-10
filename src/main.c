#include "load.h"
#include "loop.h"
#include "priv.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static void usage(void) {
	fprintf(stderr, "usage: tundra-networkd [--version]\n");
}

int main(int argc, char *argv[]) {
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("tundra-networkd %s\n", TUNDRA_NETWORKD_VERSION);
		return 0;
	}
	if (argc != 1) {
		usage();
		return 2;
	}

	static struct config cfg;
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	if (priv_shed() < 0)
		return 1;
	if (conf_load(TUNDRA_NETWORKD_CONF_DIR, &cfg) < 0)
		return 1;
	return loop_run(&cfg);
}
