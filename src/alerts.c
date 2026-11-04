#include "alerts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void alerts_init(struct alert_store *s) {
	memset(s, 0, sizeof(*s));
	s->next_id = 1;
}

static void drop_at(struct alert_store *s, int i) {
	memmove(&s->items[i], &s->items[i + 1],
	        (size_t)(s->n - i - 1) * sizeof(s->items[0]));
	s->n--;
}

unsigned int alerts_add(struct alert_store *s, const char *profile,
                        const struct alert *a, const char *line) {
	if (s->n == ALERTS_MAX) {
		int victim = 0;
		for (int i = 0; i < s->n; i++)
			if (s->items[i].a.action != AL_STOP) {
				victim = i;
				break;
			}
		drop_at(s, victim);
	}
	struct stored_alert *sa = &s->items[s->n++];
	memset(sa, 0, sizeof(*sa));
	sa->id = s->next_id++;
	snprintf(sa->profile, sizeof(sa->profile), "%s",
	         profile && *profile ? profile : "-");
	sa->a = *a;
	snprintf(sa->line, sizeof(sa->line), "%s", line);
	return sa->id;
}

const struct stored_alert *alerts_find(const struct alert_store *s,
                                       unsigned int id) {
	for (int i = 0; i < s->n; i++)
		if (s->items[i].id == id)
			return &s->items[i];
	return NULL;
}

void alerts_remove(struct alert_store *s, unsigned int id) {
	for (int i = 0; i < s->n; i++)
		if (s->items[i].id == id) {
			drop_at(s, i);
			return;
		}
}

int alerts_format(const struct stored_alert *sa, char *out, size_t size) {
	int n =
	    snprintf(out, size, "ALERT %u %s %s", sa->id, sa->profile, sa->line);
	return n < 0 || (size_t)n >= size ? -1 : n;
}

static int load_line(struct alert_store *s, const char *line) {
	char profile[PROFILE_NAME_MAX + 1];
	unsigned int id;
	int used = 0;
	if (sscanf(line, "ALERT %u %64s %n", &id, profile, &used) != 2 || !used ||
	    id == 0 || s->n == ALERTS_MAX || alerts_find(s, id))
		return -1;
	struct stored_alert *sa = &s->items[s->n];
	memset(sa, 0, sizeof(*sa));
	if (alert_parse(line + used, &sa->a) < 0 ||
	    strlen(line + used) >= sizeof(sa->line))
		return -1;
	sa->id = id;
	snprintf(sa->profile, sizeof(sa->profile), "%s", profile);
	snprintf(sa->line, sizeof(sa->line), "%s", line + used);
	s->n++;
	if (id >= s->next_id)
		s->next_id = id + 1;
	return 0;
}

int alerts_load(struct alert_store *s, const char *text, size_t len) {
	char line[ALERT_LINE_MAX + 128];
	int loaded = 0;
	const char *end = text + len;
	for (const char *p = text; p < end;) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = (size_t)((nl ? nl : end) - p);
		if (n < sizeof(line)) {
			memcpy(line, p, n);
			line[n] = '\0';
			loaded += load_line(s, line) == 0;
		}
		p += n + 1;
	}
	return loaded;
}

int alerts_ids_on(const struct alert_store *s, const char *ifname, char *out,
                  size_t size) {
	size_t used = 0;
	out[0] = '\0';
	for (int i = 0; i < s->n; i++) {
		if (strcmp(s->items[i].a.on, ifname) != 0)
			continue;
		int w = snprintf(out + used, size - used, "%s%u", used ? "," : "",
		                 s->items[i].id);
		if (w < 0 || (size_t)w >= size - used)
			return -1;
		used += (size_t)w;
	}
	if (!used)
		snprintf(out, size, "-");
	return 0;
}

int alerts_offers(const struct stored_alert *sa, const char *option) {
	size_t len = strlen(option);
	for (const char *p = sa->a.reply; *p;) {
		size_t n = strcspn(p, ",");
		if (n == len && strncmp(p, option, len) == 0)
			return 1;
		p += n;
		if (*p == ',')
			p++;
	}
	return 0;
}
