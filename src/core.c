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
	added->s_backoff_ms = BACKOFF_MIN_MS;
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

static void begin_stop(struct core_iface *ci, int sig, uint64_t now,
                       struct actions *out) {
	ci->st = C_STOPPING;
	ci->stop_at = now + STOP_TIMEOUT_MS;
	signal_child(out, ci, sig);
}

static void sensor_signal(struct actions *out, const struct core_iface *ci,
                          int sig) {
	struct action *a = push(out, A_SIGNAL, ci);
	if (a) {
		a->kind = K_SENSOR;
		a->sig = sig;
	}
}

static void sensor_stop(struct core_iface *ci, uint64_t now,
                        struct actions *out) {
	if (ci->sst == S_RUNNING) {
		ci->sst = S_STOPPING;
		ci->s_stop_at = now + STOP_TIMEOUT_MS;
		sensor_signal(out, ci, SIGTERM);
	} else if (ci->sst == S_BACKOFF) {
		ci->sst = S_OFF;
	}
}

static void sensor_sync(struct core_iface *ci, uint64_t now,
                        struct actions *out) {
	int want = ci->st == C_RUNNING && ci->want_addr;
	if (ci->sst == S_OFF && want) {
		struct action *a = push(out, A_SPAWN_SENSOR, ci);
		if (!a)
			return;
		a->kind = K_SENSOR;
		a->addr = ci->want_addr;
		a->gw = ci->want_gw;
		ci->sst = S_RUNNING;
		ci->s_addr = ci->want_addr;
		ci->s_gw = ci->want_gw;
		ci->s_started_at = now;
	} else if (ci->sst == S_RUNNING && (!want || ci->s_addr != ci->want_addr ||
	                                    ci->s_gw != ci->want_gw)) {
		sensor_stop(ci, now, out);
	} else if (ci->sst == S_BACKOFF && !want) {
		ci->sst = S_OFF;
	}
}

static void spawn_with(struct core_iface *ci, const struct profile *p,
                       const struct iface_conf *conf, uint64_t now,
                       struct actions *out) {
	if (p != &ci->conn) {
		ci->conn = *p;
		ci->kept[0] = '\0';
	}
	if (conf != &ci->conn_conf) {
		ci->has_conn_conf = conf != NULL;
		if (conf)
			ci->conn_conf = *conf;
	}
	p = &ci->conn;
	conf = ci->has_conn_conf ? &ci->conn_conf : NULL;
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

static void spawn(struct core *c, struct core_iface *ci,
                  const struct profile *p, uint64_t now, struct actions *out) {
	const struct iface_conf *conf;
	managed(c, ci, &conf);
	spawn_with(ci, p, conf, now, out);
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
	if (ci->kept[0] && !c->shutting_down) {
		spawn_with(ci, &ci->conn, ci->has_conn_conf ? &ci->conn_conf : NULL,
		           now, out);
		return;
	}
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
	if (c->lockdown && ci->st == C_IDLE) {
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
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

	if (ci->st == C_STOPPING && ci->alert_stop) {
		ci->alert_stop = 0;
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
		return;
	}
	if (ci->st == C_STOPPING) {
		ci->st = C_IDLE;
		ci->kept[0] = '\0';
		if (c->shutting_down)
			return;
		const struct iface_conf *conf;
		const struct profile *p =
		    ci->pending[0] ? profile_by_name(c, ci->pending) : NULL;
		ci->pending[0] = '\0';
		if (p && matches(p, ci) && managed(c, ci, &conf))
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
		if (ci->st == C_STOPPING && ci->stop_at && now >= ci->stop_at) {
			ci->stop_at = 0;
			signal_child(out, ci, SIGKILL);
			log_msg(out, ci, "tundra-dhcpc did not stop in %u s, killed",
			        STOP_TIMEOUT_MS / 1000);
		}
		if (ci->sst == S_BACKOFF && now >= ci->s_retry_at) {
			ci->sst = S_OFF;
			sensor_sync(ci, now, out);
		}
		if (ci->sst == S_STOPPING && ci->s_stop_at && now >= ci->s_stop_at) {
			ci->s_stop_at = 0;
			sensor_signal(out, ci, SIGKILL);
			log_msg(out, ci, "tundra-blueice-arp did not stop in %u s, killed",
			        STOP_TIMEOUT_MS / 1000);
		}
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
		begin_stop(ci, SIGUSR1, now, out);
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

int core_disconnect(struct core *c, const char *ifname, uint64_t now,
                    struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return -1;
	ci->manual_off = 1;
	ci->pending[0] = '\0';
	switch (ci->st) {
	case C_RUNNING:
		begin_stop(ci, SIGUSR1, now, out);
		break;
	case C_BACKOFF:
	case C_WAIT_LINK:
		ci->st = C_IDLE;
		ci->kept[0] = '\0';
		break;
	case C_IDLE:
	case C_STOPPING:
	case C_BLOCKED:
		break;
	}
	return 0;
}

void core_shutdown(struct core *c, uint64_t now, struct actions *out) {
	c->shutting_down = 1;
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->st == C_RUNNING) {
			begin_stop(ci, SIGTERM, now, out);
		} else if (ci->st == C_BACKOFF || ci->st == C_WAIT_LINK) {
			ci->st = C_IDLE;
		}
		sensor_stop(ci, now, out);
	}
}

int core_done(const struct core *c) {
	if (!c->shutting_down)
		return 0;
	for (int i = 0; i < c->n; i++)
		if (c->ifs[i].st == C_RUNNING || c->ifs[i].st == C_STOPPING ||
		    c->ifs[i].sst == S_RUNNING || c->ifs[i].sst == S_STOPPING)
			return 0;
	return 1;
}

static void earliest(uint64_t *next, uint64_t t) {
	if (t && t < *next)
		*next = t;
}

uint64_t core_next_deadline(const struct core *c) {
	uint64_t next = UINT64_MAX;
	for (int i = 0; i < c->n; i++) {
		const struct core_iface *ci = &c->ifs[i];
		if (ci->st == C_BACKOFF)
			earliest(&next, ci->retry_at);
		else if (ci->st == C_STOPPING)
			earliest(&next, ci->stop_at);
		if (ci->sst == S_BACKOFF)
			earliest(&next, ci->s_retry_at);
		else if (ci->sst == S_STOPPING)
			earliest(&next, ci->s_stop_at);
	}
	return next;
}

const char *core_unmanaged(const struct core *c, const char *ifname) {
	const struct core_iface *ci = core_find(c, ifname);
	const struct iface_conf *conf;
	if (!ci || managed(c, ci, &conf))
		return NULL;
	return conf ? conf->error : c->cfg->ifaces.error;
}

static int same_but_dns(const struct profile *a, const struct profile *b) {
	return strcmp(a->interface, b->interface) == 0 &&
	       a->any_interface == b->any_interface && a->has_ssid == b->has_ssid &&
	       a->ssid_len == b->ssid_len &&
	       memcmp(a->ssid, b->ssid, a->ssid_len) == 0 &&
	       a->security == b->security &&
	       a->has_passphrase == b->has_passphrase &&
	       strcmp(a->passphrase, b->passphrase) == 0 &&
	       a->address == b->address && a->ip == b->ip &&
	       a->prefix == b->prefix && a->has_gateway == b->has_gateway &&
	       a->gateway == b->gateway && a->autoconnect == b->autoconnect &&
	       a->trusted == b->trusted;
}

static int same_conf(const struct core_iface *ci,
                     const struct iface_conf *conf) {
	static const struct iface_conf none;
	const struct iface_conf *a = ci->has_conn_conf ? &ci->conn_conf : &none;
	const struct iface_conf *b = conf ? conf : &none;
	return a->has_metric == b->has_metric &&
	       (!a->has_metric || a->metric == b->metric) &&
	       a->has_table == b->has_table &&
	       (!a->has_table || strcmp(a->table, b->table) == 0) &&
	       a->has_grace == b->has_grace &&
	       (!a->has_grace || a->grace_ms == b->grace_ms);
}

static void keep(struct core_iface *ci, const char *why, struct actions *out) {
	snprintf(ci->kept, sizeof(ci->kept), "%s", why);
	log_msg(out, ci, "%s, keeping the previous configuration", why);
}

static void reload_iface(struct core *c, struct core_iface *ci, uint64_t now,
                         struct actions *out) {
	if (ci->st == C_BLOCKED && ci->blocked != B_ALERT) {
		ci->st = C_IDLE;
		ci->blocked = B_NONE;
		return;
	}
	if (ci->st != C_RUNNING && ci->st != C_BACKOFF && ci->st != C_WAIT_LINK)
		return;

	const struct profile *p = profile_by_name(c, ci->profile);
	const struct iface_conf *conf;
	int ok = managed(c, ci, &conf);
	char why[CONF_ERR_MAX];
	if (!p) {
		ci->kept[0] = '\0';
		log_msg(out, ci, "profile %s removed, disconnecting", ci->profile);
		if (ci->st == C_RUNNING)
			begin_stop(ci, SIGUSR1, now, out);
		else
			ci->st = C_IDLE;
		return;
	}
	if (!p->valid) {
		snprintf(why, sizeof(why), "profile %s has errors", p->name);
		keep(ci, why, out);
		return;
	}
	if (!ok) {
		keep(ci, "interfaces has errors", out);
		return;
	}
	ci->kept[0] = '\0';
	if (same_but_dns(&ci->conn, p) && same_conf(ci, conf)) {
		ci->conn = *p;
		return;
	}
	if (ci->st != C_RUNNING)
		return;
	if (matches(p, ci)) {
		log_msg(out, ci, "profile %s changed, reconnecting", p->name);
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
	} else {
		log_msg(out, ci, "profile %s no longer matches, disconnecting",
		        p->name);
	}
	begin_stop(ci, SIGUSR1, now, out);
}

void core_reload(struct core *c, const struct config *cfg, uint64_t now,
                 struct actions *out) {
	c->cfg = cfg;
	if (c->shutting_down)
		return;
	for (int i = 0; i < c->n; i++)
		reload_iface(c, &c->ifs[i], now, out);
	for (int i = 0; i < c->n; i++)
		try_autoconnect(c, &c->ifs[i], now, out);
}

void core_lease(struct core *c, const char *ifname, uint32_t addr, uint32_t gw,
                uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	if (!addr)
		ci->conflict_addr = 0;
	else if (addr == ci->conflict_addr)
		addr = 0;
	ci->want_addr = addr;
	ci->want_gw = addr ? gw : 0;
	if (!c->shutting_down)
		sensor_sync(ci, now, out);
}

static void stop_one(struct core_iface *ci, uint64_t now, struct actions *out) {
	ci->pending[0] = '\0';
	if (ci->st == C_RUNNING) {
		ci->alert_stop = 1;
		begin_stop(ci, SIGUSR1, now, out);
	} else if (ci->st == C_STOPPING) {
		ci->alert_stop = 1;
	} else {
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
	}
	ci->want_addr = 0;
	sensor_stop(ci, now, out);
}

void core_stop(struct core *c, const char *ifname, int level, uint64_t now,
               struct actions *out) {
	if (level >= ALERT_ALL_LEVEL) {
		c->lockdown = 1;
		for (int i = 0; i < c->n; i++) {
			stop_one(&c->ifs[i], now, out);
			log_msg(out, &c->ifs[i], "stopped by an alert of level %d on %s",
			        level, ifname);
		}
		return;
	}
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	stop_one(ci, now, out);
	log_msg(out, ci, "stopped by an alert of level %d", level);
}

void core_sensor_exit(struct core *c, const char *ifname, int status,
                      int conflict, uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || (ci->sst != S_RUNNING && ci->sst != S_STOPPING))
		return;
	if (ci->sst == S_STOPPING) {
		ci->sst = S_OFF;
		ci->s_stop_at = 0;
		if (!c->shutting_down)
			sensor_sync(ci, now, out);
		return;
	}
	ci->sst = S_OFF;
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (code == 1 && conflict) {
		ci->conflict_addr = ci->s_addr;
		ci->want_addr = 0;
		log_msg(out, ci, "address conflict, asking tundra-dhcpc for another");
		if (ci->st == C_RUNNING)
			signal_child(out, ci, SIGUSR2);
		return;
	}
	if (code == 111)
		return;

	if (now - ci->s_started_at >= STABLE_MS) {
		ci->s_fails = 0;
		ci->s_backoff_ms = BACKOFF_MIN_MS;
	}
	ci->s_fails++;
	if (code < 0)
		log_msg(out, ci, "tundra-blueice-arp killed by signal %d",
		        WTERMSIG(status));
	else
		log_msg(out, ci, "tundra-blueice-arp exited with %d", code);
	if (ci->s_fails >= SENSOR_FAILS_MAX) {
		log_msg(out, ci, "tundra-blueice-arp failed %d times in a row",
		        ci->s_fails);
		core_stop(c, ci->name, SENSOR_DOWN_LEVEL, now, out);
		return;
	}
	ci->sst = S_BACKOFF;
	ci->s_retry_at = now + ci->s_backoff_ms;
	log_msg(out, ci, "relaunching tundra-blueice-arp in %u ms",
	        ci->s_backoff_ms);
	ci->s_backoff_ms *= 2;
	if (ci->s_backoff_ms > BACKOFF_MAX_MS)
		ci->s_backoff_ms = BACKOFF_MAX_MS;
}
