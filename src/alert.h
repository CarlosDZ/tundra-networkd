#ifndef TUNDRA_ALERT_H
#define TUNDRA_ALERT_H

#define ALERT_TOKEN_MAX 64

enum alert_action { AL_STOP, AL_WARN, AL_REINFORCE };
enum alert_source { SRC_DHCPC, SRC_SENSOR };

struct alert {
	int level;
	enum alert_action action;
	char by[ALERT_TOKEN_MAX + 1];
	char on[ALERT_TOKEN_MAX + 1];
	char reply[ALERT_TOKEN_MAX + 1];
	char kase[ALERT_TOKEN_MAX + 1];
};

int alert_parse(const char *line, struct alert *a);
int alert_allowed(enum alert_source src, const char *by);

#endif
