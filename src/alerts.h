#ifndef TUNDRA_ALERTS_H
#define TUNDRA_ALERTS_H

#include "alert.h"
#include "profile.h"

#include <stddef.h>

#define ALERTS_MAX 64
#define ALERT_LINE_MAX 512

struct stored_alert {
	unsigned int id;
	char profile[PROFILE_NAME_MAX + 1];
	struct blueice_parsed a;
	char line[ALERT_LINE_MAX];
};

struct alert_store {
	struct stored_alert items[ALERTS_MAX];
	int n;
	unsigned int next_id;
};

void alerts_init(struct alert_store *s);
/* When the store is full an alert leaves to make room: *dropped gets its id,
 * 0 otherwise. */
unsigned int alerts_add(struct alert_store *s, const char *profile,
                        const struct blueice_parsed *a, const char *line,
                        unsigned int *dropped);
const struct stored_alert *alerts_find(const struct alert_store *s,
                                       unsigned int id);
void alerts_remove(struct alert_store *s, unsigned int id);
int alerts_format(const struct stored_alert *sa, char *out, size_t size);
int alerts_load(struct alert_store *s, const char *text, size_t len);
int alerts_ids_on(const struct alert_store *s, const char *ifname, char *out,
                  size_t size);
int alerts_offers(const struct stored_alert *sa, const char *option);

#endif
