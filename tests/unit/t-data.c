#include "conf/grammar.c"
#include "conf/profile.c"
#include "test.h"

static char text[16384];

static size_t slurp(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	return n;
}

static int examples(int *valid) {
	char block[4096];
	size_t used = 0;
	int n = 0, closes = 0, inside = 0;
	*valid = 0;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (!inside && strcmp(line, "# match {") != 0)
			continue;
		inside = 1;
		const char *body = line[0] == '#' ? line + 1 : line;
		if (*body == ' ')
			body++;
		used +=
		    (size_t)snprintf(block + used, sizeof(block) - used, "%s\n", body);
		closes += strcmp(body, "}") == 0;
		if (closes == 2) {
			struct profile p;
			n++;
			*valid += profile_parse(&p, "example", block, used, 0) == 0;
			used = 0;
			closes = 0;
			inside = 0;
		}
	}
	return n;
}

int main(void) {
	struct profile p;
	size_t n = slurp("data/999-dhcp.tnc");
	CHECK(n && profile_parse(&p, "999-dhcp", text, n, 0) == 0 &&
	          p.any_interface && p.address == ADDR_DHCP && p.dns == DNS_LEASE &&
	          p.autoconnect && !p.trusted,
	      "999-dhcp.tnc is the fallback of ADR-026");

	n = slurp("data/000-template.tnc");
	struct grammar g;
	CHECK(n && gr_parse(text, n, &g) == 0 && g.n == 0,
	      "000-template.tnc is all comments");
	int valid, found = examples(&valid);
	CHECK(found == 3 && valid == 3,
	      "every example in the template is a valid profile");
	return test_summary();
}
