#include "alert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *next_token(const char **p, size_t *len) {
	while (**p == ' ')
		(*p)++;
	if (!**p)
		return NULL;
	const char *start = *p;
	*len = strcspn(start, " ");
	*p += *len;
	return start;
}

static int copy(char out[ALERT_TOKEN_MAX + 1], const char *s, size_t len) {
	if (len == 0 || len > ALERT_TOKEN_MAX)
		return -1;
	memcpy(out, s, len);
	out[len] = '\0';
	return 0;
}

static int is(const char *s, size_t len, const char *word) {
	return len == strlen(word) && strncmp(s, word, len) == 0;
}

int alert_parse(const char *line, struct alert *a) {
	const char *p = line, *t;
	size_t len;
	memset(a, 0, sizeof(*a));

	if (!(t = next_token(&p, &len)) || !is(t, len, "alert"))
		return -1;
	if (!(t = next_token(&p, &len)) || len < 5 || len > 6 ||
	    strncmp(t, "111/", 4) != 0)
		return -1;
	char lvl[3] = {0};
	memcpy(lvl, t + 4, len - 4);
	char *end;
	long level = strtol(lvl, &end, 10);
	if (*end || level < 1 || level > 10 || (lvl[0] == '0'))
		return -1;
	a->level = (int)level;

	if (!(t = next_token(&p, &len)))
		return -1;
	if (is(t, len, "stop"))
		a->action = AL_STOP;
	else if (is(t, len, "warn"))
		a->action = AL_WARN;
	else if (is(t, len, "reinforce"))
		a->action = AL_REINFORCE;
	else
		return -1;

	int in_info = 0;
	while ((t = next_token(&p, &len)) && !is(t, len, "--")) {
		if (!in_info && is(t, len, "info")) {
			in_info = 1;
			continue;
		}
		const char *key = t;
		size_t klen = len;
		if (!(t = next_token(&p, &len)) || is(t, len, "--"))
			return -1;
		char *dst = NULL;
		if (!in_info && is(key, klen, "by"))
			dst = a->by;
		else if (!in_info && is(key, klen, "on"))
			dst = a->on;
		else if (!in_info && is(key, klen, "reply"))
			dst = a->reply;
		else if (in_info && is(key, klen, "case"))
			dst = a->kase;
		if (dst && (dst[0] || copy(dst, t, len) < 0))
			return -1;
	}
	return a->by[0] && a->on[0] && a->reply[0] && a->kase[0] ? 0 : -1;
}

int alert_allowed(enum alert_source src, const char *by) {
	if (strcmp(by, "tundra-blueice-arp") == 0)
		return 1;
	return src == SRC_DHCPC && strcmp(by, "tundra-dhcpc") == 0;
}
