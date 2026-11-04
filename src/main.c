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
	usage();
	return 2;
}
