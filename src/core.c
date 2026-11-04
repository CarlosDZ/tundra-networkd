#include "core.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static struct action *push(struct actions *out, enum act_type type,
                           const struct core_iface *ci) {
	if (out->n == ACTIONS_MAX)
		return NULL;
	struct action *a = &out->a[out->n++];
	memset(a, 0, sizeof(*a));
	a->type = type;
	snprintf(a->iface, sizeof(a->iface), "%s", ci->name);
	return a;
}

static void log_msg(struct actions *out, const struct core_iface *ci,
                    const char *fmt, ...) {
	struct action *a = push(out, A_LOG, ci);
	if (!a)
		return;
	char msg[sizeof(a->msg) - IF_NAMESIZE - 2];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(a->msg, sizeof(a->msg), "%s: %s", ci->name, msg);
}

static void signal_child(struct actions *out, const struct core_iface *ci,
                         int sig) {
	struct action *a = push(out, A_SIGNAL, ci);
	if (a)
		a->sig = sig;
}

const struct core_iface *core_find(const struct core *c, const char *ifname) {
	for (int i = 0; i < c->n; i++)
		if (strcmp(c->ifs[i].name, ifname) == 0)
			return &c->ifs[i];
	return NULL;
}

static struct core_iface *find(struct core *c, const char *ifname) {
	return (struct core_iface *)core_find(c, ifname);
}

static struct core_iface *find_or_add(struct core *c, const char *ifname) {
	struct core_iface *found = find(c, ifname);
	if (found)
		return found;
	if (c->n == CORE_IFACES)
		return NULL;
	struct core_iface *added = &c->ifs[c->n++];
	memset(added, 0, sizeof(*added));
	size_t len = strnlen(ifname, IF_NAMESIZE - 1);
	memcpy(added->name, ifname, len);
	added->name[len] = '\0';
	added->backoff_ms = BACKOFF_MIN_MS;
	return added;
}

static const struct profile *profile_by_name(const struct core *c,
                                             const char *name) {
	for (int i = 0; i < c->cfg->n; i++)
		if (strcmp(c->cfg->profiles[i].name, name) == 0)
			return &c->cfg->profiles[i];
	return NULL;
}

static int managed(const struct core *c, const struct core_iface *ci,
                   const struct iface_conf **conf) {
	*conf = NULL;
	if (!c->cfg->ifaces.valid)
		return 0;
	*conf = ifaces_find(&c->cfg->ifaces, ci->name);
	return !*conf || (*conf)->valid;
}

static int matches(const struct profile *p, const struct core_iface *ci) {
	if (!p->valid || p->has_ssid)
		return 0;
	if (p->any_interface)
		return ci->physical;
	return strcmp(p->interface, ci->name) == 0;
}

static void spawn(struct core *c, struct core_iface *ci,
                  const struct profile *p, uint64_t now, struct actions *out) {
	const struct iface_conf *conf;
	managed(c, ci, &conf);
	snprintf(ci->profile, sizeof(ci->profile), "%s", p->name);
	ci->pending[0] = '\0';

	if (p->address == ADDR_STATIC) {
		ci->st = C_BLOCKED;
		ci->blocked = B_UNSUPPORTED;
		log_msg(out, ci, "profile %s: static addressing is not implemented yet",
		        p->name);
		return;
	}

	struct action *a = push(out, A_SPAWN_DHCPC, ci);
	if (!a)
		return;
	snprintf(a->profile, sizeof(a->profile), "%s", p->name);
	if (conf) {
		a->has_metric = conf->has_metric;
		a->metric = conf->metric;
		a->has_table = conf->has_table;
		snprintf(a->table, sizeof(a->table), "%s", conf->table);
		a->grace_ms = conf->grace_ms;
	}
	ci->st = C_RUNNING;
	ci->started_at = now;
}

static const struct profile *autoselect(const struct core *c,
                                        const struct core_iface *ci) {
	for (int i = 0; i < c->cfg->n; i++) {
		const struct profile *p = &c->cfg->profiles[i];
		if (p->autoconnect && matches(p, ci))
			return p;
	}
	return NULL;
}

static void try_autoconnect(struct core *c, struct core_iface *ci, uint64_t now,
                            struct actions *out) {
	const struct iface_conf *conf;
	if (c->shutting_down || !ci->present || ci->st != C_IDLE ||
	    ci->manual_off || ci->blocked != B_NONE || !managed(c, ci, &conf))
		return;
	const struct profile *p = autoselect(c, ci);
	if (p)
		spawn(c, ci, p, now, out);
}

static void resume(struct core *c, struct core_iface *ci, uint64_t now,
                   struct actions *out) {
	const struct profile *p = profile_by_name(c, ci->profile);
	const struct iface_conf *conf;
	ci->st = C_IDLE;
	if (p && matches(p, ci) && managed(c, ci, &conf) && !c->shutting_down)
		spawn(c, ci, p, now, out);
	else
		try_autoconnect(c, ci, now, out);
}

void core_init(struct core *c, const struct config *cfg) {
	memset(c, 0, sizeof(*c));
	c->cfg = cfg;
}

void core_link(struct core *c, const char *ifname, int physical, uint64_t now,
               struct actions *out) {
	struct core_iface *ci = find_or_add(c, ifname);
	if (!ci)
		return;
	int was = ci->present;
	ci->present = 1;
	ci->physical = physical;

	const struct iface_conf *conf;
	if (!was && !managed(c, ci, &conf)) {
		if (!c->cfg->ifaces.valid)
			log_msg(out, ci, "not managed: interfaces: %s",
			        c->cfg->ifaces.error);
		else
			log_msg(out, ci, "not managed: %s", conf->error);
	}
	if (ci->st == C_WAIT_LINK)
		resume(c, ci, now, out);
	else
		try_autoconnect(c, ci, now, out);
}

void core_link_gone(struct core *c, const char *ifname, uint64_t now,
                    struct actions *out) {
	(void)now;
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	ci->present = 0;
	if (ci->st == C_BACKOFF) {
		ci->st = C_WAIT_LINK;
		log_msg(out, ci, "removed, waiting for it to come back");
	}
}

static void on_failure(struct core *c, struct core_iface *ci, uint64_t now,
                       struct actions *out) {
	(void)c;
	if (!ci->present) {
		ci->st = C_WAIT_LINK;
		log_msg(out, ci, "removed, waiting for it to come back");
		return;
	}
	if (now - ci->started_at >= STABLE_MS)
		ci->backoff_ms = BACKOFF_MIN_MS;
	ci->st = C_BACKOFF;
	ci->retry_at = now + ci->backoff_ms;
	log_msg(out, ci, "relaunching tundra-dhcpc in %u ms", ci->backoff_ms);
	ci->backoff_ms *= 2;
	if (ci->backoff_ms > BACKOFF_MAX_MS)
		ci->backoff_ms = BACKOFF_MAX_MS;
}

void core_child_exit(struct core *c, const char *ifname, int status,
                     uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || (ci->st != C_RUNNING && ci->st != C_STOPPING))
		return;

	if (ci->st == C_STOPPING) {
		ci->st = C_IDLE;
		if (c->shutting_down)
			return;
		const struct profile *p =
		    ci->pending[0] ? profile_by_name(c, ci->pending) : NULL;
		if (p)
			spawn(c, ci, p, now, out);
		else
			try_autoconnect(c, ci, now, out);
		return;
	}

	if (WIFSIGNALED(status)) {
		log_msg(out, ci, "tundra-dhcpc killed by signal %d", WTERMSIG(status));
		on_failure(c, ci, now, out);
		return;
	}

	switch (WEXITSTATUS(status)) {
	case 0:
		ci->st = C_BLOCKED;
		ci->blocked = B_EXITED;
		log_msg(out, ci,
		        "tundra-dhcpc exited without being asked to, not "
		        "relaunching");
		return;
	case 2:
		ci->st = C_BLOCKED;
		ci->blocked = B_CONFIG;
		log_msg(out, ci, "profile %s rejected, waiting for a reload",
		        ci->profile);
		return;
	case 111:
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
		log_msg(out, ci, "alert on profile %s, waiting for the user",
		        ci->profile);
		return;
	default:
		log_msg(out, ci, "tundra-dhcpc exited with %d", WEXITSTATUS(status));
		on_failure(c, ci, now, out);
		return;
	}
}

void core_timer(struct core *c, uint64_t now, struct actions *out) {
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->st == C_BACKOFF && now >= ci->retry_at)
			resume(c, ci, now, out);
	}
}

enum connect_result core_connect(struct core *c, const char *profile,
                                 const char *ifname, uint64_t now,
                                 struct actions *out) {
	const struct profile *p = profile_by_name(c, profile);
	if (!p)
		return CONNECT_NO_PROFILE;
	if (!p->valid)
		return CONNECT_INVALID;
	struct core_iface *ci = find(c, ifname);
	if (!ci || !ci->present)
		return CONNECT_NO_IFACE;
	const struct iface_conf *conf;
	if (!managed(c, ci, &conf))
		return CONNECT_UNMANAGED;
	if (!matches(p, ci))
		return CONNECT_NO_MATCH;
	if (ci->blocked == B_CONFIG)
		return CONNECT_BLOCKED;
	if (ci->blocked == B_ALERT && strcmp(ci->profile, p->name) == 0)
		return CONNECT_ALERT_PENDING;

	ci->manual_off = 0;
	switch (ci->st) {
	case C_RUNNING:
		if (strcmp(ci->profile, p->name) == 0)
			return CONNECT_OK;
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
		ci->st = C_STOPPING;
		signal_child(out, ci, SIGUSR1);
		return CONNECT_OK;
	case C_STOPPING:
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
		return CONNECT_OK;
	case C_IDLE:
	case C_BACKOFF:
	case C_WAIT_LINK:
	case C_BLOCKED:
		break;
	}
	ci->st = C_IDLE;
	ci->blocked = B_NONE;
	ci->backoff_ms = BACKOFF_MIN_MS;
	spawn(c, ci, p, now, out);
	return CONNECT_OK;
}

int core_disconnect(struct core *c, const char *ifname, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return -1;
	ci->manual_off = 1;
	ci->pending[0] = '\0';
	switch (ci->st) {
	case C_RUNNING:
		ci->st = C_STOPPING;
		signal_child(out, ci, SIGUSR1);
		break;
	case C_BACKOFF:
	case C_WAIT_LINK:
		ci->st = C_IDLE;
		break;
	case C_IDLE:
	case C_STOPPING:
	case C_BLOCKED:
		break;
	}
	return 0;
}

void core_shutdown(struct core *c, struct actions *out) {
	c->shutting_down = 1;
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->st == C_RUNNING) {
			ci->st = C_STOPPING;
			signal_child(out, ci, SIGTERM);
		} else if (ci->st == C_BACKOFF || ci->st == C_WAIT_LINK) {
			ci->st = C_IDLE;
		}
	}
}

int core_done(const struct core *c) {
	if (!c->shutting_down)
		return 0;
	for (int i = 0; i < c->n; i++)
		if (c->ifs[i].st == C_RUNNING || c->ifs[i].st == C_STOPPING)
			return 0;
	return 1;
}

uint64_t core_next_deadline(const struct core *c) {
	uint64_t next = UINT64_MAX;
	for (int i = 0; i < c->n; i++)
		if (c->ifs[i].st == C_BACKOFF && c->ifs[i].retry_at < next)
			next = c->ifs[i].retry_at;
	return next;
}
