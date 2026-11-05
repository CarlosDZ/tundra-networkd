#include "core.h"

#include <arpa/inet.h>
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

static void child_signal(struct actions *out, const struct core_iface *ci,
                         enum child_kind kind, int sig) {
	struct action *a = push(out, A_SIGNAL, ci);
	if (a) {
		a->kind = kind;
		a->sig = sig;
	}
}

static void signal_child(struct actions *out, const struct core_iface *ci,
                         int sig) {
	child_signal(out, ci, K_DHCPC, sig);
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
	added->w_backoff_ms = BACKOFF_MIN_MS;
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
	return !*conf || ((*conf)->valid && (ci->wireless || !(*conf)->has_wifi));
}

static const char *unmanaged_why(const struct core *c,
                                 const struct iface_conf *conf) {
	if (!conf)
		return c->cfg->ifaces.error;
	return conf->valid ? conf->wired_error : conf->error;
}

static int wifi_off(const struct core *c, const struct core_iface *ci) {
	const struct iface_conf *conf;
	return ci->wireless && managed(c, ci, &conf) && conf && conf->has_wifi &&
	       !conf->wifi;
}

static int wifi_matches(const struct profile *p, const struct core_iface *ci) {
	return p->valid && p->has_ssid && ci->wireless &&
	       (p->any_interface || strcmp(p->interface, ci->name) == 0);
}

static void wpa_resync(const struct core_iface *ci, struct actions *out) {
	if (ci->wst == W_RUNNING)
		push(out, A_WPA_SYNC, ci);
}

static int fallback_matches(const struct profile *p,
                            const struct core_iface *ci) {
	if (!p->valid || p->has_ssid)
		return 0;
	if (p->any_interface)
		return ci->physical;
	return strcmp(p->interface, ci->name) == 0;
}

/* A profile without ssid connects a cable by itself, but on WiFi it only
 * applies once associated to a network without a profile. */
static int matches(const struct profile *p, const struct core_iface *ci) {
	return !ci->wireless && fallback_matches(p, ci);
}

static int same_ssid(const struct profile *p, const unsigned char *ssid,
                     size_t len) {
	return p->ssid_len == len && memcmp(p->ssid, ssid, len) == 0;
}

/* Whether p can carry the connection the interface has now. */
static int fits(const struct profile *p, const struct core_iface *ci) {
	if (!ci->wireless)
		return matches(p, ci);
	if (p->has_ssid)
		return wifi_matches(p, ci) && same_ssid(p, ci->cssid, ci->cssid_len);
	return fallback_matches(p, ci);
}

static void stop_child(struct core_iface *ci, enum child_kind kind, int sig,
                       uint64_t now, struct actions *out) {
	ci->st = C_STOPPING;
	ci->stop_kind = kind;
	ci->stop_at = now + STOP_TIMEOUT_MS;
	child_signal(out, ci, kind, sig);
}

static void begin_stop(struct core_iface *ci, int sig, uint64_t now,
                       struct actions *out) {
	stop_child(ci, K_DHCPC, sig, now, out);
}

static int has_child(const struct core_iface *ci) {
	return ci->st == C_RUNNING || ci->st == C_PROBING || ci->st == C_STATIC;
}

static void end_conn(struct core_iface *ci) {
	ci->naccepts = 0;
	ci->dispute = 0;
	ci->override = 0;
	ci->s_fails = 0;
	ci->s_backoff_ms = BACKOFF_MIN_MS;
}

static int accepted(const struct core_iface *ci, const char *kase, int level) {
	for (int i = 0; i < ci->naccepts; i++)
		if (strcmp(ci->accepts[i].kase, kase) == 0 &&
		    ci->accepts[i].level >= level)
			return 1;
	return 0;
}

static void add_accept(struct core_iface *ci, const char *kase, int level) {
	for (int i = 0; i < ci->naccepts; i++)
		if (strcmp(ci->accepts[i].kase, kase) == 0) {
			if (level > ci->accepts[i].level)
				ci->accepts[i].level = level;
			return;
		}
	if (ci->naccepts == ACCEPTS_MAX)
		return;
	struct accept *a = &ci->accepts[ci->naccepts++];
	snprintf(a->kase, sizeof(a->kase), "%s", kase);
	a->level = level;
}

static int is_dispute(const char *kase) {
	return strcmp(kase, "A") == 0 || strcmp(kase, "B") == 0 ||
	       strcmp(kase, "C") == 0 || strcmp(kase, "mac-addr") == 0;
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
	int want = (ci->st == C_RUNNING || ci->st == C_STATIC) && ci->want_addr;
	if (ci->sst == S_OFF && want) {
		struct action *a = push(out, A_SPAWN_SENSOR, ci);
		if (!a)
			return;
		a->kind = K_SENSOR;
		a->addr = ci->want_addr;
		a->gw = ci->want_gw;
		a->defend_always = ci->dispute;
		size_t used = 0;
		for (int i = 0; i < ci->naccepts; i++) {
			int w = snprintf(a->accept + used, sizeof(a->accept) - used,
			                 "%s%s/%d", i ? "," : "", ci->accepts[i].kase,
			                 ci->accepts[i].level);
			if (w < 0 || (size_t)w >= sizeof(a->accept) - used)
				break;
			used += (size_t)w;
		}
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

static void wpa_stop(struct core_iface *ci, uint64_t now, struct actions *out) {
	if (ci->wst == W_RUNNING) {
		ci->wst = W_STOPPING;
		ci->w_stop_at = now + STOP_TIMEOUT_MS;
		child_signal(out, ci, K_WPA, SIGTERM);
	} else if (ci->wst == W_BACKOFF) {
		ci->wst = W_OFF;
	}
}

/* An interface whose block turned invalid keeps its wpa_supplicant, as the
 * connection keeps the previous configuration. */
static void wpa_sync(struct core *c, struct core_iface *ci, uint64_t now,
                     struct actions *out) {
	const struct iface_conf *conf;
	int live = !c->shutting_down && ci->present && ci->wireless;
	if (live && !managed(c, ci, &conf))
		return;
	if (!live || wifi_off(c, ci)) {
		wpa_stop(ci, now, out);
		return;
	}
	if (ci->wst != W_OFF)
		return;
	struct action *a = push(out, A_SPAWN_WPA, ci);
	if (!a)
		return;
	a->kind = K_WPA;
	ci->wst = W_RUNNING;
	ci->w_started_at = now;
}

static void keep_down(const struct core *c, const struct core_iface *ci,
                      struct actions *out) {
	if (!ci->present || !ci->up || !wifi_off(c, ci))
		return;
	log_msg(out, ci, "wifi is off, bringing the interface down");
	push(out, A_LINK_DOWN, ci);
}

static void push_static(const struct core_iface *ci, enum act_type type,
                        struct actions *out) {
	struct action *a = push(out, type, ci);
	if (!a)
		return;
	a->addr = ci->conn.ip;
	a->prefix = ci->conn.prefix;
	a->gw = ci->conn.has_gateway ? ci->conn.gateway : 0;
	if (ci->has_conn_conf) {
		a->has_metric = ci->conn_conf.has_metric;
		a->metric = ci->conn_conf.metric;
		a->has_table = ci->conn_conf.has_table;
		snprintf(a->table, sizeof(a->table), "%s", ci->conn_conf.table);
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
		ci->started_at = now;
		ci->want_addr = 0;
		if (ci->dispute) {
			push_static(ci, A_STATIC_APPLY, out);
			ci->st = C_STATIC;
			return;
		}
		struct action *a = push(out, A_SPAWN_PROBE, ci);
		if (!a)
			return;
		a->kind = K_PROBE;
		a->addr = p->ip;
		ci->st = C_PROBING;
		return;
	}

	struct action *a = push(out, A_SPAWN_DHCPC, ci);
	if (!a)
		return;
	snprintf(a->profile, sizeof(a->profile), "%s", p->name);
	a->probe_off = ci->dispute;
	if (conf) {
		a->has_metric = conf->has_metric;
		a->metric = conf->metric;
		a->has_table = conf->has_table;
		snprintf(a->table, sizeof(a->table), "%s", conf->table);
	}
	a->grace_ms = conf && conf->has_grace ? conf->grace_ms
	              : ci->wireless          ? WIFI_GRACE_MS
	                                      : 0;
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
	    ci->manual_off || ci->blocked != B_NONE || !managed(c, ci, &conf) ||
	    wifi_off(c, ci))
		return;
	const struct profile *p = autoselect(c, ci);
	if (p) {
		end_conn(ci);
		spawn(c, ci, p, now, out);
	}
}

static void resume(struct core *c, struct core_iface *ci, uint64_t now,
                   struct actions *out) {
	const struct profile *p = profile_by_name(c, ci->profile);
	const struct iface_conf *conf;
	ci->st = C_IDLE;
	if (wifi_off(c, ci))
		return;
	if (ci->kept[0] && !c->shutting_down) {
		spawn_with(ci, &ci->conn, ci->has_conn_conf ? &ci->conn_conf : NULL,
		           now, out);
		return;
	}
	if (p && fits(p, ci) && managed(c, ci, &conf) && !c->shutting_down)
		spawn(c, ci, p, now, out);
	else
		try_autoconnect(c, ci, now, out);
}

static void after_stop(struct core *c, struct core_iface *ci, uint64_t now,
                       struct actions *out) {
	ci->stop_at = 0;
	if (ci->alert_stop) {
		ci->alert_stop = 0;
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
		return;
	}
	ci->st = C_IDLE;
	ci->kept[0] = '\0';
	if (c->shutting_down)
		return;
	const struct iface_conf *conf;
	const struct profile *p =
	    ci->pending[0] ? profile_by_name(c, ci->pending) : NULL;
	ci->pending[0] = '\0';
	if (p && fits(p, ci) && managed(c, ci, &conf) && !wifi_off(c, ci))
		spawn(c, ci, p, now, out);
	else
		try_autoconnect(c, ci, now, out);
}

static void release(struct core *c, struct core_iface *ci, uint64_t now,
                    struct actions *out) {
	if (ci->st == C_RUNNING) {
		begin_stop(ci, SIGUSR1, now, out);
	} else if (ci->st == C_PROBING) {
		stop_child(ci, K_PROBE, SIGTERM, now, out);
	} else if (ci->st == C_STATIC) {
		push_static(ci, A_STATIC_REMOVE, out);
		ci->want_addr = 0;
		sensor_sync(ci, now, out);
		ci->st = C_STOPPING;
		after_stop(c, ci, now, out);
	}
}

static void taken(struct core_iface *ci, uint32_t ip, const char *mac,
                  const char *src, struct actions *out) {
	char addr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &ip, addr, sizeof(addr));
	ci->st = C_BLOCKED;
	ci->blocked = B_TAKEN;
	log_msg(out, ci, "static address %s in use by %s", addr, mac);
	struct action *a = push(out, A_ALERT, ci);
	if (!a)
		return;
	a->alert = AL_WARN;
	a->level = STATIC_TAKEN_LEVEL;
	snprintf(a->kase, sizeof(a->kase), "static-taken");
	snprintf(a->msg, sizeof(a->msg), "ip %s mac %.32s%s%.32s", addr, mac,
	         src ? " src " : "", src ? src : "");
}

void core_init(struct core *c, const struct config *cfg) {
	memset(c, 0, sizeof(*c));
	c->cfg = cfg;
}

void core_link(struct core *c, const char *ifname, unsigned int flags,
               uint64_t now, struct actions *out) {
	struct core_iface *ci = find_or_add(c, ifname);
	if (!ci)
		return;
	int was = ci->present;
	ci->present = 1;
	ci->physical = (flags & LINK_PHYSICAL) != 0;
	ci->wireless = (flags & LINK_WIRELESS) != 0;
	ci->up = (flags & LINK_UP) != 0;

	const struct iface_conf *conf;
	if (!was && !managed(c, ci, &conf)) {
		if (!c->cfg->ifaces.valid)
			log_msg(out, ci, "not managed: interfaces: %s",
			        c->cfg->ifaces.error);
		else
			log_msg(out, ci, "not managed: %s", unmanaged_why(c, conf));
	}
	wpa_sync(c, ci, now, out);
	keep_down(c, ci, out);
	if (c->lockdown && ci->st == C_IDLE) {
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
		ci->stop_id = c->lockdown_id;
	}
	if (ci->st == C_WAIT_LINK)
		resume(c, ci, now, out);
	else
		try_autoconnect(c, ci, now, out);
}

void core_link_gone(struct core *c, const char *ifname, uint64_t now,
                    struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	ci->present = 0;
	ci->associated = 0;
	wpa_sync(c, ci, now, out);
	if (ci->st == C_STATIC) {
		ci->st = C_WAIT_LINK;
		ci->want_addr = 0;
		sensor_sync(ci, now, out);
		log_msg(out, ci, "removed, waiting for it to come back");
	} else if (ci->st == C_BACKOFF) {
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
	if (ci->conn.address == ADDR_STATIC)
		log_msg(out, ci, "retrying the static address in %u ms",
		        ci->backoff_ms);
	else
		log_msg(out, ci, "relaunching tundra-dhcpc in %u ms", ci->backoff_ms);
	ci->backoff_ms *= 2;
	if (ci->backoff_ms > BACKOFF_MAX_MS)
		ci->backoff_ms = BACKOFF_MAX_MS;
}

void core_child_exit(struct core *c, const char *ifname, int status,
                     uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || (ci->st != C_RUNNING &&
	            (ci->st != C_STOPPING || ci->stop_kind != K_DHCPC)))
		return;
	if (ci->st == C_STOPPING) {
		after_stop(c, ci, now, out);
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
			child_signal(out, ci, ci->stop_kind, SIGKILL);
			log_msg(out, ci, "%s did not stop in %u s, killed",
			        ci->stop_kind == K_DHCPC ? "tundra-dhcpc"
			                                 : "tundra-blueice-arp",
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
		if (ci->wst == W_BACKOFF && now >= ci->w_retry_at) {
			ci->wst = W_OFF;
			wpa_sync(c, ci, now, out);
		}
		if (ci->wst == W_STOPPING && ci->w_stop_at && now >= ci->w_stop_at) {
			ci->w_stop_at = 0;
			child_signal(out, ci, K_WPA, SIGKILL);
			log_msg(out, ci, "wpa_supplicant did not stop in %u s, killed",
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
	if (wifi_off(c, ci))
		return CONNECT_OFF;
	if (p->has_ssid) {
		if (!wifi_matches(p, ci))
			return CONNECT_NO_MATCH;
		ci->manual_off = 0;
		snprintf(ci->wselect, sizeof(ci->wselect), "%s", p->name);
		wpa_resync(ci, out);
		return CONNECT_OK;
	}
	if (!matches(p, ci))
		return CONNECT_NO_MATCH;
	if (ci->blocked == B_CONFIG)
		return CONNECT_BLOCKED;
	if (ci->blocked == B_ALERT && strcmp(ci->profile, p->name) == 0)
		return CONNECT_ALERT_PENDING;

	int same = strcmp(ci->profile, p->name) == 0;
	if (!has_child(ci) || !same) {
		end_conn(ci);
		ci->stop_id = 0;
	}
	ci->manual_off = 0;
	switch (ci->st) {
	case C_RUNNING:
	case C_PROBING:
	case C_STATIC:
		if (same)
			return CONNECT_OK;
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
		release(c, ci, now, out);
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
	ci->wselect[0] = '\0';
	end_conn(ci);
	if (ci->wireless)
		wpa_resync(ci, out);
	switch (ci->st) {
	case C_RUNNING:
	case C_PROBING:
	case C_STATIC:
		release(c, ci, now, out);
		break;
	case C_BACKOFF:
	case C_WAIT_LINK:
		ci->st = C_IDLE;
		ci->kept[0] = '\0';
		break;
	case C_BLOCKED:
		if (ci->blocked != B_ALERT) {
			ci->st = C_IDLE;
			ci->blocked = B_NONE;
		}
		break;
	case C_IDLE:
	case C_STOPPING:
		break;
	}
	return 0;
}

void core_shutdown(struct core *c, uint64_t now, struct actions *out) {
	c->shutting_down = 1;
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->st == C_RUNNING)
			begin_stop(ci, SIGTERM, now, out);
		else if (ci->st == C_PROBING)
			stop_child(ci, K_PROBE, SIGTERM, now, out);
		else if (ci->st == C_BACKOFF || ci->st == C_WAIT_LINK ||
		         ci->st == C_STATIC)
			ci->st = C_IDLE;
		sensor_stop(ci, now, out);
		wpa_stop(ci, now, out);
	}
}

int core_done(const struct core *c) {
	if (!c->shutting_down)
		return 0;
	for (int i = 0; i < c->n; i++)
		if (c->ifs[i].st == C_RUNNING || c->ifs[i].st == C_STOPPING ||
		    c->ifs[i].sst == S_RUNNING || c->ifs[i].sst == S_STOPPING ||
		    c->ifs[i].wst == W_RUNNING || c->ifs[i].wst == W_STOPPING)
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
		if (ci->wst == W_BACKOFF)
			earliest(&next, ci->w_retry_at);
		else if (ci->wst == W_STOPPING)
			earliest(&next, ci->w_stop_at);
	}
	return next;
}

const char *core_unmanaged(const struct core *c, const char *ifname) {
	const struct core_iface *ci = core_find(c, ifname);
	const struct iface_conf *conf;
	if (!ci || managed(c, ci, &conf))
		return NULL;
	return unmanaged_why(c, conf);
}

int core_wifi_off(const struct core *c, const char *ifname) {
	const struct core_iface *ci = core_find(c, ifname);
	return ci && wifi_off(c, ci);
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
	if (ci->st == C_BLOCKED && ci->blocked != B_ALERT &&
	    ci->blocked != B_TAKEN) {
		ci->st = C_IDLE;
		ci->blocked = B_NONE;
		return;
	}
	if (!has_child(ci) && ci->st != C_BACKOFF && ci->st != C_WAIT_LINK)
		return;

	const struct profile *p = profile_by_name(c, ci->profile);
	const struct iface_conf *conf;
	int ok = managed(c, ci, &conf);
	char why[CONF_ERR_MAX];
	if (!p) {
		ci->kept[0] = '\0';
		end_conn(ci);
		log_msg(out, ci, "profile %s removed, disconnecting", ci->profile);
		if (has_child(ci))
			release(c, ci, now, out);
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
	end_conn(ci);
	if (!has_child(ci))
		return;
	if (fits(p, ci)) {
		log_msg(out, ci, "profile %s changed, reconnecting", p->name);
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
	} else {
		log_msg(out, ci, "profile %s no longer matches, disconnecting",
		        p->name);
	}
	release(c, ci, now, out);
}

static void reload_wifi(struct core *c, struct core_iface *ci, uint64_t now,
                        struct actions *out) {
	if (wifi_off(c, ci) && has_child(ci)) {
		log_msg(out, ci, "wifi is off, disconnecting");
		ci->kept[0] = '\0';
		ci->pending[0] = '\0';
		end_conn(ci);
		release(c, ci, now, out);
	} else if (wifi_off(c, ci) &&
	           (ci->st == C_BACKOFF || ci->st == C_WAIT_LINK ||
	            (ci->st == C_BLOCKED && ci->blocked != B_ALERT))) {
		ci->st = C_IDLE;
		ci->blocked = B_NONE;
	}
	wpa_sync(c, ci, now, out);
	keep_down(c, ci, out);
	const struct profile *sel =
	    ci->wselect[0] ? profile_by_name(c, ci->wselect) : NULL;
	if (ci->wselect[0] && !(sel && wifi_matches(sel, ci)))
		ci->wselect[0] = '\0';
	if (ci->wireless)
		wpa_resync(ci, out);
}

void core_reload(struct core *c, const struct config *cfg, uint64_t now,
                 struct actions *out) {
	c->cfg = cfg;
	if (c->shutting_down)
		return;
	for (int i = 0; i < c->n; i++)
		reload_iface(c, &c->ifs[i], now, out);
	for (int i = 0; i < c->n; i++)
		reload_wifi(c, &c->ifs[i], now, out);
	for (int i = 0; i < c->n; i++)
		try_autoconnect(c, &c->ifs[i], now, out);
}

void core_lease(struct core *c, const char *ifname, uint32_t addr, uint32_t gw,
                uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || ci->st == C_PROBING || ci->st == C_STATIC)
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

static void stop_one(struct core *c, struct core_iface *ci, int level,
                     unsigned int id, uint64_t now, struct actions *out) {
	ci->pending[0] = '\0';
	if (level >= TRUST_LEVEL && ci->st != C_IDLE && !ci->override)
		ci->override = id;
	ci->stop_id = id;
	if (has_child(ci)) {
		ci->alert_stop = 1;
		release(c, ci, now, out);
	} else if (ci->st == C_STOPPING) {
		ci->alert_stop = 1;
	} else {
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
	}
	ci->want_addr = 0;
	sensor_stop(ci, now, out);
}

void core_stop(struct core *c, const char *ifname, int level, unsigned int id,
               uint64_t now, struct actions *out) {
	if (level >= ALERT_ALL_LEVEL) {
		c->lockdown = 1;
		c->lockdown_id = id;
		for (int i = 0; i < c->n; i++) {
			stop_one(c, &c->ifs[i], level, id, now, out);
			log_msg(out, &c->ifs[i], "stopped by an alert of level %d on %s",
			        level, ifname);
		}
		return;
	}
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	stop_one(c, ci, level, id, now, out);
	log_msg(out, ci, "stopped by an alert of level %d", level);
}

void core_note_alert(struct core *c, const char *ifname, int level,
                     unsigned int id) {
	struct core_iface *ci = find(c, ifname);
	if (ci && level >= TRUST_LEVEL && ci->st != C_IDLE && !ci->override)
		ci->override = id;
}

int core_force(struct core *c, unsigned int id, const char *origin,
               const char *kase, int level, uint64_t now, struct actions *out) {
	int n = 0;
	if (c->lockdown && c->lockdown_id == id) {
		c->lockdown = 0;
		c->lockdown_id = 0;
	}
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->stop_id != id)
			continue;
		n++;
		ci->stop_id = 0;
		if (strcmp(ci->name, origin) == 0) {
			add_accept(ci, kase, level);
			if (is_dispute(kase))
				ci->dispute = 1;
			ci->s_fails = 0;
			ci->s_backoff_ms = BACKOFF_MIN_MS;
		}
		if (ci->st == C_STOPPING) {
			ci->alert_stop = 0;
			memcpy(ci->pending, ci->profile, sizeof(ci->pending));
			continue;
		}
		if (ci->st != C_BLOCKED || ci->blocked != B_ALERT)
			continue;
		ci->st = C_IDLE;
		ci->blocked = B_NONE;
		const struct profile *p =
		    ci->profile[0] ? profile_by_name(c, ci->profile) : NULL;
		const struct iface_conf *conf;
		if (!p || !fits(p, ci) || !managed(c, ci, &conf) || wifi_off(c, ci))
			try_autoconnect(c, ci, now, out);
		else if (!ci->present)
			ci->st = C_WAIT_LINK;
		else
			spawn(c, ci, p, now, out);
		log_msg(out, ci, "alert %u forced, reconnecting", id);
	}
	return n;
}

int core_release_alert(struct core *c, unsigned int id, struct actions *out) {
	int n = 0;
	if (c->lockdown && c->lockdown_id == id) {
		c->lockdown = 0;
		c->lockdown_id = 0;
	}
	for (int i = 0; i < c->n; i++) {
		struct core_iface *ci = &c->ifs[i];
		if (ci->stop_id != id)
			continue;
		n++;
		end_conn(ci);
		ci->stop_id = 0;
		ci->manual_off = 1;
		ci->pending[0] = '\0';
		if (ci->st == C_STOPPING) {
			ci->alert_stop = 0;
		} else if (ci->st == C_BLOCKED && ci->blocked == B_ALERT) {
			ci->st = C_IDLE;
			ci->blocked = B_NONE;
		}
		log_msg(out, ci, "alert %u answered with disconnect", id);
	}
	return n;
}

void core_restore_stop(struct core *c, const char *ifname, const char *profile,
                       int level, unsigned int id) {
	if (level >= ALERT_ALL_LEVEL) {
		c->lockdown = 1;
		c->lockdown_id = id;
	}
	struct core_iface *ci = find_or_add(c, ifname);
	if (!ci)
		return;
	ci->st = C_BLOCKED;
	ci->blocked = B_ALERT;
	ci->stop_id = id;
	if (strcmp(profile, "-") != 0) {
		const struct profile *p = profile_by_name(c, profile);
		if (p)
			ci->conn = *p;
		else
			snprintf(ci->conn.name, sizeof(ci->conn.name), "%s", profile);
		snprintf(ci->profile, sizeof(ci->profile), "%s", profile);
		if (level >= TRUST_LEVEL)
			ci->override = id;
	}
}

void core_sensor_exit(struct core *c, const char *ifname, int status,
                      const char *conflict_mac, uint64_t now,
                      struct actions *out) {
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
	if (code == 1 && conflict_mac && ci->st == C_STATIC) {
		push_static(ci, A_STATIC_REMOVE, out);
		ci->want_addr = 0;
		taken(ci, ci->s_addr, conflict_mac, NULL, out);
		return;
	}
	if (code == 1 && conflict_mac) {
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
		int covered = accepted(ci, "sensor-down", SENSOR_DOWN_LEVEL);
		struct action *a = push(out, A_ALERT, ci);
		if (a) {
			a->alert = covered ? AL_WARN : AL_STOP;
			a->level = SENSOR_DOWN_LEVEL;
			snprintf(a->kase, sizeof(a->kase), "sensor-down");
			snprintf(a->msg, sizeof(a->msg), "fails %d", ci->s_fails);
		}
		if (!covered)
			return;
		ci->s_fails = 0;
	}
	ci->sst = S_BACKOFF;
	ci->s_retry_at = now + ci->s_backoff_ms;
	log_msg(out, ci, "relaunching tundra-blueice-arp in %u ms",
	        ci->s_backoff_ms);
	ci->s_backoff_ms *= 2;
	if (ci->s_backoff_ms > BACKOFF_MAX_MS)
		ci->s_backoff_ms = BACKOFF_MAX_MS;
}

void core_probe_exit(struct core *c, const char *ifname, int status,
                     enum probe_result result, const char *mac, const char *src,
                     uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	if (ci->st == C_STOPPING && ci->stop_kind == K_PROBE) {
		after_stop(c, ci, now, out);
		return;
	}
	if (ci->st != C_PROBING)
		return;
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (code == 0 && result == PROBE_FREE) {
		push_static(ci, A_STATIC_APPLY, out);
		ci->st = C_STATIC;
	} else if (code == 0 && result == PROBE_TAKEN) {
		taken(ci, ci->conn.ip, mac && *mac ? mac : "-", src, out);
	} else if (code == 2) {
		ci->st = C_BLOCKED;
		ci->blocked = B_CONFIG;
		log_msg(out, ci, "probe rejected profile %s, waiting for a reload",
		        ci->profile);
	} else if (code == 111) {
		ci->st = C_BLOCKED;
		ci->blocked = B_ALERT;
	} else {
		if (code < 0)
			log_msg(out, ci, "probe killed by signal %d", WTERMSIG(status));
		else
			log_msg(out, ci, "probe exited with %d", code);
		on_failure(c, ci, now, out);
	}
}

void core_static_result(struct core *c, const char *ifname, int rc,
                        uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || ci->st != C_STATIC)
		return;
	if (rc == 0) {
		ci->want_addr = ci->conn.ip;
		ci->want_gw = ci->conn.has_gateway ? ci->conn.gateway : 0;
		log_msg(out, ci, "static address of profile %s applied", ci->profile);
		sensor_sync(ci, now, out);
	} else if (rc == STATIC_USAGE) {
		ci->st = C_BLOCKED;
		ci->blocked = B_CONFIG;
		log_msg(out, ci, "tundra-ip rejected profile %s, waiting for a reload",
		        ci->profile);
	} else {
		log_msg(out, ci, "could not apply the static address");
		on_failure(c, ci, now, out);
	}
}

void core_wpa_exit(struct core *c, const char *ifname, int status, uint64_t now,
                   struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || (ci->wst != W_RUNNING && ci->wst != W_STOPPING))
		return;
	if (ci->wst == W_STOPPING) {
		ci->wst = W_OFF;
		ci->w_stop_at = 0;
		wpa_sync(c, ci, now, out);
		return;
	}
	core_wifi_lost(c, ifname);
	if (WIFSIGNALED(status))
		log_msg(out, ci, "wpa_supplicant killed by signal %d",
		        WTERMSIG(status));
	else
		log_msg(out, ci, "wpa_supplicant exited with %d", WEXITSTATUS(status));
	if (!ci->present || c->shutting_down) {
		ci->wst = W_OFF;
		return;
	}
	if (now - ci->w_started_at >= STABLE_MS)
		ci->w_backoff_ms = BACKOFF_MIN_MS;
	ci->wst = W_BACKOFF;
	ci->w_retry_at = now + ci->w_backoff_ms;
	log_msg(out, ci, "relaunching wpa_supplicant in %u ms", ci->w_backoff_ms);
	ci->w_backoff_ms *= 2;
	if (ci->w_backoff_ms > BACKOFF_MAX_MS)
		ci->w_backoff_ms = BACKOFF_MAX_MS;
}

int core_wifi_nets(const struct core *c, const char *ifname,
                   struct wifi_net *out, int max, int *disconnected) {
	const struct core_iface *ci = core_find(c, ifname);
	int total = 0, n = 0;
	*disconnected = ci && ci->manual_off;
	if (!ci)
		return 0;
	for (int i = 0; i < c->cfg->n; i++)
		total += wifi_matches(&c->cfg->profiles[i], ci);
	for (int i = 0; i < c->cfg->n && n < max; i++) {
		const struct profile *p = &c->cfg->profiles[i];
		if (!wifi_matches(p, ci))
			continue;
		out[n].p = p;
		out[n].priority = total - n;
		out[n].enabled =
		    ci->wselect[0] ? strcmp(p->name, ci->wselect) == 0 : p->autoconnect;
		n++;
	}
	return n;
}

void core_wifi_lost(struct core *c, const char *ifname) {
	struct core_iface *ci = find(c, ifname);
	if (!ci)
		return;
	ci->associated = 0;
	ci->assid_len = 0;
	ci->bssid[0] = '\0';
}

static const struct profile *
assoc_profile(const struct core *c, const struct core_iface *ci,
              const char *id_str, const unsigned char *ssid, size_t len) {
	const struct profile *p = id_str[0] ? profile_by_name(c, id_str) : NULL;
	if (p && wifi_matches(p, ci) && same_ssid(p, ssid, len))
		return p;
	for (int i = 0; i < c->cfg->n; i++)
		if (fallback_matches(&c->cfg->profiles[i], ci))
			return &c->cfg->profiles[i];
	return NULL;
}

void core_wifi_assoc(struct core *c, const char *ifname, const char *id_str,
                     const unsigned char *ssid, size_t ssid_len,
                     const char *bssid, uint64_t now, struct actions *out) {
	struct core_iface *ci = find(c, ifname);
	if (!ci || !ci->wireless || ssid_len > SSID_MAX)
		return;
	ci->associated = 1;
	memcpy(ci->assid, ssid, ssid_len);
	ci->assid_len = ssid_len;
	snprintf(ci->bssid, sizeof(ci->bssid), "%s", bssid);
	if (c->shutting_down)
		return;
	const struct profile *p = assoc_profile(c, ci, id_str, ssid, ssid_len);
	if (!p) {
		log_msg(out, ci, "associated, but no profile fits that network");
		return;
	}
	int same = ci->st != C_IDLE && strcmp(ci->profile, p->name) == 0 &&
	           ci->cssid_len == ssid_len &&
	           memcmp(ci->cssid, ssid, ssid_len) == 0;
	if (same || (ci->st == C_BLOCKED && ci->blocked == B_ALERT))
		return;
	memcpy(ci->cssid, ssid, ssid_len);
	ci->cssid_len = ssid_len;
	if (has_child(ci) || ci->st == C_STOPPING) {
		log_msg(out, ci,
		        "associated to another network, reconnecting with profile %s",
		        p->name);
		snprintf(ci->pending, sizeof(ci->pending), "%s", p->name);
		if (has_child(ci)) {
			end_conn(ci);
			release(c, ci, now, out);
		}
		return;
	}
	end_conn(ci);
	ci->st = C_IDLE;
	ci->blocked = B_NONE;
	ci->backoff_ms = BACKOFF_MIN_MS;
	spawn(c, ci, p, now, out);
}
