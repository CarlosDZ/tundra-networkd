#include "loop.h"
#include "alert.h"
#include "alerts.h"
#include "core.h"
#include "files.h"
#include "lease.h"
#include "leaseinfo.h"
#include "link.h"
#include "proto.h"
#include "resolv.h"
#include "server.h"
#include "spawn.h"
#include "static.h"
#include "wpa.h"
#include "wpactl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_arp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILDREN_MAX (CORE_IFACES * 3)
#define OUT_LINE_MAX 512
#define DHCPC_LEASE_DIR "/run/tundra/dhcpc"
#define LEASE_FILE_MAX 1024
#define RESOLV_PATH TUNDRA_NETWORKD_RUN_DIR "/resolv.conf"
#define T_SIGNAL 1
#define T_LINK 2
#define T_LEASE 3
#define T_CHILD 4
#define T_LISTEN 5
#define T_CLIENT 6
#define T_WPA_DIR 7
#define T_WPA 8
#define REPLY_MAX 768
#define ALERTS_PATH TUNDRA_NETWORKD_RUN_DIR "/alerts"
#define ALERT_ID_PATH TUNDRA_NETWORKD_RUN_DIR "/alert-id"
#define ALERTS_FILE_MAX (ALERTS_MAX * REPLY_MAX)
#define WPA_DIR TUNDRA_NETWORKD_RUN_DIR "/wpa"
#define WPA_EVENT_MAX 4096
#define WPA_CMD_MAX (WPA_VAR_MAX + 32)

struct child {
	int used;
	pid_t pid;
	int fd;
	int reaped;
	enum child_kind kind;
	char conflict_mac[32];
	enum probe_result probe;
	char mac[32];
	char src[32];
	char ifname[IF_NAMESIZE];
	char profile[PROFILE_NAME_MAX + 1];
	char line[OUT_LINE_MAX];
	size_t len;
	struct wpa_conn wpa;
};

struct wpa_net {
	char name[PROFILE_NAME_MAX + 1];
	int id;
	uint64_t hash;
	int priority;
	int enabled;
};

struct wpa_table {
	struct wpa_net items[PROFILES_MAX];
	int n;
	int disconnected;
};

struct conn_view {
	int connected;
	char profile[PROFILE_NAME_MAX + 1];
	char address[LEASEINFO_ADDR_MAX];
};

struct daemon {
	struct config *cfg;
	struct config *spare;
	struct core core;
	int ep;
	int sfd;
	int lfd;
	int ifd;
	int wfd;
	struct child kids[CHILDREN_MAX];
	struct server srv;
	struct conn_view view[CORE_IFACES];
	struct alert_store alerts;
	struct wpa_table wnets[CORE_IFACES];
	int dirty;
	int dns_written;
	char dns[RESOLV_TEXT_MAX];
};

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int watch_fd(struct daemon *d, int fd, uint64_t tag) {
	struct epoll_event ev = {.events = EPOLLIN, .data.u64 = tag};
	if (epoll_ctl(d->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
		perror("epoll_ctl error");
		return -1;
	}
	return 0;
}

static void execute(struct daemon *d, struct actions *acts);

static int read_file(const char *path, char *buf, size_t size, size_t *len) {
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	*len = 0;
	ssize_t n;
	while (*len < size && (n = read(fd, buf + *len, size - *len)) != 0) {
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			close(fd);
			return -1;
		}
		*len += (size_t)n;
	}
	close(fd);
	return 0;
}

static void save_alerts(struct daemon *d) {
	static char text[ALERTS_FILE_MAX];
	char line[REPLY_MAX], id[16];
	size_t used = 0;
	for (int i = 0; i < d->alerts.n; i++) {
		int n = alerts_format(&d->alerts.items[i], line, sizeof(line));
		if (n < 0 || used + (size_t)n + 1 > sizeof(text))
			continue;
		memcpy(text + used, line, (size_t)n);
		used += (size_t)n;
		text[used++] = '\n';
	}
	int n = snprintf(id, sizeof(id), "%u\n", d->alerts.next_id);
	if (files_make_dirs(TUNDRA_NETWORKD_RUN_DIR, 0755) < 0 ||
	    files_write_atomic(ALERTS_PATH, text, used, 0600) < 0 ||
	    files_write_atomic(ALERT_ID_PATH, id, (size_t)n, 0600) < 0)
		fprintf(stderr, "networkd: cannot save the alerts: %s\n",
		        strerror(errno));
}

static void load_alerts(struct daemon *d) {
	static char text[ALERTS_FILE_MAX];
	char id[16];
	size_t len;
	alerts_init(&d->alerts);
	if (read_file(ALERTS_PATH, text, sizeof(text), &len) == 0)
		alerts_load(&d->alerts, text, len);
	if (read_file(ALERT_ID_PATH, id, sizeof(id) - 1, &len) == 0) {
		id[len] = '\0';
		unsigned long next = strtoul(id, NULL, 10);
		if (next > d->alerts.next_id && next < 0xffffffffUL)
			d->alerts.next_id = (unsigned int)next;
	}
	for (int i = 0; i < d->alerts.n; i++) {
		const struct stored_alert *sa = &d->alerts.items[i];
		if (sa->a.action == BLUEICE_STOP)
			core_restore_stop(&d->core, sa->a.on, sa->profile, sa->a.level,
			                  sa->id);
	}
	if (d->alerts.n)
		printf("networkd: %d pending alerts restored\n", d->alerts.n);
}

static void alert_closed(struct daemon *d, unsigned int id, const char *how) {
	char line[REPLY_MAX];
	if (proto_alert_closed(id, how, line, sizeof(line)) >= 0)
		server_broadcast(&d->srv, line);
}

static void take_alert(struct daemon *d, const char *ifname,
                       const struct blueice_parsed *a, const char *line) {
	const struct core_iface *ci = core_find(&d->core, ifname);
	const char *profile = ci && ci->st != C_IDLE ? ci->profile : "-";
	struct actions acts = {0};
	char fwd[REPLY_MAX];
	unsigned int dropped;
	unsigned int id = alerts_add(&d->alerts, profile, a, line, &dropped);
	save_alerts(d);
	if (dropped) {
		printf("networkd: alert %u dropped, too many pending\n", dropped);
		alert_closed(d, dropped, "dropped");
	}
	printf("networkd: %s: alert %u recorded\n", ifname, id);
	if (alerts_format(alerts_find(&d->alerts, id), fwd, sizeof(fwd)) >= 0)
		server_broadcast(&d->srv, fwd);
	core_note_alert(&d->core, ifname, a->level, id);
	if (a->action == BLUEICE_STOP) {
		core_stop(&d->core, ifname, a->level, id, now_ms(), &acts);
		execute(d, &acts);
	}
	d->dirty = 1;
}

static const char *own_text(const char *kase) {
	if (strcmp(kase, "sensor-down") == 0)
		return "tundra-blueice-arp keeps failing";
	if (strcmp(kase, "static-taken") == 0)
		return "static address in use";
	return kase;
}

/* act->msg carries the info as "<key> <value>" pairs. */
static int own_line(char *line, size_t size, const struct action *act,
                    uint64_t ts_ms) {
	struct blueice_alert b;
	char pairs[sizeof(act->msg)];
	char *save;
	if (blueice_alert_init(&b, "tundra-networkd", act->kase, act->level,
	                       own_text(act->kase)) < 0)
		return -1;
	snprintf(pairs, sizeof(pairs), "%s", act->msg);
	for (char *key = strtok_r(pairs, " ", &save); key;
	     key = strtok_r(NULL, " ", &save)) {
		char *value = strtok_r(NULL, " ", &save);
		if (!value)
			return -1;
		blueice_alert_add(&b, key, value);
	}
	return blueice_alert_format(line, size, &b, act->alert, act->iface, ts_ms);
}

static void own_alert(struct daemon *d, const struct action *act) {
	struct timespec ts;
	char line[OUT_LINE_MAX];
	struct blueice_parsed a;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t ts_ms =
	    (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	if (own_line(line, sizeof(line), act, ts_ms) < 0) {
		printf("networkd: %s: could not format the %s alert\n", act->iface,
		       act->kase);
		return;
	}
	printf("networkd: %s\n", line);
	if (blueice_alert_parse(line, &a) == 0)
		take_alert(d, act->iface, &a, line);
}

static void handle_alert(struct daemon *d, struct child *k) {
	const char *from =
	    k->kind == K_DHCPC ? "tundra-dhcpc" : "tundra-blueice-arp";
	struct blueice_parsed a;
	if (blueice_alert_parse(k->line, &a) < 0) {
		printf("networkd: %s: malformed alert from %s discarded\n", k->ifname,
		       from);
		return;
	}
	if (!alert_allowed(k->kind == K_DHCPC ? SRC_DHCPC : SRC_SENSOR, a.by)) {
		printf("networkd: %s: alert by %s discarded on the pipe of %s\n",
		       k->ifname, a.by, from);
		return;
	}
	if (strcmp(a.on, k->ifname) != 0) {
		printf("networkd: %s: alert on %s discarded on the pipe of %s\n",
		       k->ifname, a.on, from);
		return;
	}
	take_alert(d, k->ifname, &a, k->line);
}

static void emit_line(struct daemon *d, struct child *k) {
	k->line[k->len] = '\0';
	k->len = 0;
	if (k->kind == K_WPA) {
		k->line[strcspn(k->line, "\r")] = '\0';
		printf("wpa[%s]: %s\n", k->ifname, k->line);
		return;
	}
	if (k->kind == K_DHCPC)
		printf("dhcpc[%s/%s]: %s\n", k->ifname, k->profile, k->line);
	else
		printf("blueice-arp[%s]: %s\n", k->ifname, k->line);
	if (strncmp(k->line, "alert ", 6) == 0)
		handle_alert(d, k);
	else if (k->kind == K_SENSOR &&
	         strncmp(k->line, "conflict addr ", 14) == 0 &&
	         sscanf(k->line, "conflict addr %*s mac %31s", k->conflict_mac) !=
	             1)
		snprintf(k->conflict_mac, sizeof(k->conflict_mac), "-");
	else if (k->kind == K_PROBE && strcmp(k->line, "free") == 0)
		k->probe = PROBE_FREE;
	else if (k->kind == K_PROBE &&
	         sscanf(k->line, "taken mac %31s src %31s", k->mac, k->src) >= 1)
		k->probe = PROBE_TAKEN;
}

static void release_child(struct daemon *d, struct child *k) {
	if (k->fd >= 0) {
		if (k->len)
			emit_line(d, k);
		epoll_ctl(d->ep, EPOLL_CTL_DEL, k->fd, NULL);
		close(k->fd);
		k->fd = -1;
	}
	if (k->reaped)
		k->used = 0;
}

static void drain(struct daemon *d, struct child *k) {
	char buf[1024];
	while (k->fd >= 0) {
		ssize_t n = read(k->fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			release_child(d, k);
			return;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || k->len == OUT_LINE_MAX - 1) {
				emit_line(d, k);
				if (buf[i] == '\n')
					continue;
			}
			k->line[k->len++] = buf[i];
		}
	}
}

static struct child *running_child(struct daemon *d, const char *ifname,
                                   enum child_kind kind) {
	for (int i = 0; i < CHILDREN_MAX; i++) {
		struct child *k = &d->kids[i];
		if (k->used && !k->reaped && k->kind == kind &&
		    strcmp(k->ifname, ifname) == 0)
			return k;
	}
	return NULL;
}

static struct child *new_child(struct daemon *d, const char *path,
                               char *const argv[], enum child_kind kind,
                               const char *ifname, const char *profile) {
	struct child *k = NULL;
	for (int i = 0; i < CHILDREN_MAX && !k; i++)
		if (!d->kids[i].used)
			k = &d->kids[i];
	int fd = -1;
	pid_t pid = !k              ? -1
	            : kind == K_WPA ? spawn_child_tty(path, argv, &fd)
	                            : spawn_child(path, argv, &fd);
	if (pid < 0)
		return NULL;
	memset(k, 0, sizeof(*k));
	k->used = 1;
	k->pid = pid;
	k->fd = fd;
	k->kind = kind;
	k->wpa.ctl = k->wpa.mon = -1;
	snprintf(k->ifname, sizeof(k->ifname), "%s", ifname);
	snprintf(k->profile, sizeof(k->profile), "%s", profile);
	watch_fd(d, fd, ((uint64_t)T_CHILD << 32) | (uint64_t)(k - d->kids));
	return k;
}

static void spawn_probe(struct daemon *d, const struct action *a) {
	char addr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &a->addr, addr, sizeof(addr));
	char *argv[] = {"tundra-blueice-arp", "probe", (char *)a->iface, addr,
	                NULL};
	struct child *k =
	    new_child(d, TUNDRA_BLUEICE_ARP_PATH, argv, K_PROBE, a->iface, "");
	if (!k) {
		struct actions acts = {0};
		fprintf(stderr, "networkd: %s: cannot launch tundra-blueice-arp\n",
		        a->iface);
		core_probe_exit(&d->core, a->iface, 1 << 8, PROBE_NONE, NULL, NULL,
		                now_ms(), &acts);
		execute(d, &acts);
		return;
	}
	printf("networkd: %s: probing %s (pid %d)\n", a->iface, addr, (int)k->pid);
}

static void spawn_sensor(struct daemon *d, const struct action *a) {
	char addr[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN];
	char *argv[14];
	int n = 0;
	inet_ntop(AF_INET, &a->addr, addr, sizeof(addr));
	inet_ntop(AF_INET, &a->gw, gw, sizeof(gw));
	argv[n++] = "tundra-blueice-arp";
	argv[n++] = "watch";
	argv[n++] = (char *)a->iface;
	argv[n++] = "addr";
	argv[n++] = addr;
	if (a->gw) {
		argv[n++] = "gw";
		argv[n++] = gw;
	}
	argv[n++] = "defend";
	argv[n++] = a->defend_always ? "always" : "once";
	if (a->accept[0]) {
		argv[n++] = "accept";
		argv[n++] = (char *)a->accept;
	}
	argv[n] = NULL;

	struct child *k =
	    new_child(d, TUNDRA_BLUEICE_ARP_PATH, argv, K_SENSOR, a->iface, "");
	if (!k) {
		struct actions acts = {0};
		fprintf(stderr, "networkd: %s: cannot launch tundra-blueice-arp\n",
		        a->iface);
		core_sensor_exit(&d->core, a->iface, 1 << 8, 0, now_ms(), &acts);
		execute(d, &acts);
		return;
	}
	printf("networkd: %s: tundra-blueice-arp watching %s (pid %d)\n", a->iface,
	       addr, (int)k->pid);
}

static void spawn_dhcpc(struct daemon *d, const struct action *a) {
	char metric[16], grace[16];
	char *argv[18];
	int n = 0;
	argv[n++] = "tundra-dhcpc";
	argv[n++] = (char *)a->iface;
	if (a->has_metric) {
		snprintf(metric, sizeof(metric), "%u", a->metric);
		argv[n++] = "metric";
		argv[n++] = metric;
	}
	if (a->has_table) {
		argv[n++] = "table";
		argv[n++] = (char *)a->table;
	}
	if (a->grace_ms) {
		snprintf(grace, sizeof(grace), "%u", a->grace_ms);
		argv[n++] = "carrier-grace";
		argv[n++] = grace;
	}
	if (a->probe_off) {
		argv[n++] = "probe";
		argv[n++] = "off";
	}
	argv[n++] = "lease-key";
	argv[n++] = (char *)a->profile;
	argv[n] = NULL;

	struct child *k =
	    new_child(d, TUNDRA_DHCPC_PATH, argv, K_DHCPC, a->iface, a->profile);
	if (!k) {
		struct actions acts = {0};
		fprintf(stderr, "networkd: %s: cannot launch tundra-dhcpc\n", a->iface);
		core_child_exit(&d->core, a->iface, 1 << 8, now_ms(), &acts);
		execute(d, &acts);
		return;
	}

	printf("networkd: %s: tundra-dhcpc started for profile %s (pid %d)\n",
	       a->iface, a->profile, (int)k->pid);
}

static void wpa_failed(struct daemon *d, const char *ifname) {
	struct actions acts = {0};
	core_wpa_exit(&d->core, ifname, 1 << 8, now_ms(), &acts);
	execute(d, &acts);
}

static void spawn_wpa(struct daemon *d, const struct action *a) {
	char conf[PATH_MAX], text[PATH_MAX + 64];
	snprintf(conf, sizeof(conf), "%s/%s.conf", WPA_DIR, a->iface);
	/* Without update_config: CONFIG_NO_CONFIG_WRITE rejects it (ADR-028). */
	int n = snprintf(text, sizeof(text), "ctrl_interface=%s\n", WPA_DIR);
	if (files_make_dirs(WPA_DIR, 0700) < 0 ||
	    files_write_atomic(conf, text, (size_t)n, 0600) < 0) {
		fprintf(stderr, "networkd: %s: cannot write %s: %s\n", a->iface, conf,
		        strerror(errno));
		wpa_failed(d, a->iface);
		return;
	}
	char *argv[] = {"wpa_supplicant",
	                "-i",
	                (char *)a->iface,
	                "-D",
	                "nl80211",
	                "-c",
	                conf,
	                NULL};
	struct child *k =
	    new_child(d, TUNDRA_WPA_SUPPLICANT_PATH, argv, K_WPA, a->iface, "");
	if (!k) {
		fprintf(stderr, "networkd: %s: cannot launch wpa_supplicant\n",
		        a->iface);
		wpa_failed(d, a->iface);
		return;
	}
	printf("networkd: %s: wpa_supplicant started (pid %d)\n", a->iface,
	       (int)k->pid);
}

static void wpa_detach(struct daemon *d, struct child *k) {
	if (k->wpa.mon >= 0)
		epoll_ctl(d->ep, EPOLL_CTL_DEL, k->wpa.mon, NULL);
	wpa_close(&k->wpa);
}

static struct wpa_table *wpa_table_of(struct daemon *d, const char *ifname) {
	const struct core_iface *ci = core_find(&d->core, ifname);
	return ci ? &d->wnets[ci - d->core.ifs] : NULL;
}

static int wpa_vcmd(struct child *k, char *reply, size_t size, const char *fmt,
                    va_list ap) {
	char cmd[WPA_CMD_MAX];
	int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
	if (n < 0 || (size_t)n >= sizeof(cmd) ||
	    wpa_request(k->wpa.ctl, cmd, reply, size) < 0)
		return -1;
	return strncmp(reply, "FAIL", 4) == 0 ? -1 : 0;
}

__attribute__((format(printf, 4, 5))) static int
wpa_cmd(struct child *k, char *reply, size_t size, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int r = wpa_vcmd(k, reply, size, fmt, ap);
	va_end(ap);
	return r;
}

__attribute__((format(printf, 2, 3))) static int wpa_ok(struct child *k,
                                                        const char *fmt, ...) {
	char reply[64];
	va_list ap;
	va_start(ap, fmt);
	int r = wpa_vcmd(k, reply, sizeof(reply), fmt, ap);
	va_end(ap);
	return r == 0 && strcmp(reply, "OK\n") == 0;
}

static int var_name(const char *var) { return (int)strcspn(var, " "); }

/* Adds the network and sets every variable; -1 leaves nothing behind. */
static int wpa_add(struct child *k, const struct wifi_net *want,
                   const struct wpa_network *w) {
	char reply[64];
	char *end;
	if (wpa_cmd(k, reply, sizeof(reply), "ADD_NETWORK") < 0)
		return -1;
	long id = strtol(reply, &end, 10);
	if (end == reply || *end != '\n' || id < 0 || id > 0xffff)
		return -1;
	for (int i = 0; i < w->n; i++) {
		const char *var = w->vars[i];
		if (wpa_ok(k, "SET_NETWORK %ld %s", id, var))
			continue;
		if (w->optional[i]) {
			printf("networkd: %s: wpa_supplicant without %.*s, profile %s "
			       "goes without it\n",
			       k->ifname, var_name(var), var, want->p->name);
			continue;
		}
		printf("networkd: %s: wpa_supplicant rejected %.*s of profile %s\n",
		       k->ifname, var_name(var), var, want->p->name);
		wpa_ok(k, "REMOVE_NETWORK %ld", id);
		return -1;
	}
	if (!wpa_ok(k, "SET_NETWORK %ld priority %d", id, want->priority) ||
	    (want->enabled && !wpa_ok(k, "ENABLE_NETWORK %ld", id))) {
		wpa_ok(k, "REMOVE_NETWORK %ld", id);
		return -1;
	}
	return (int)id;
}

static void wpa_sync_nets(struct daemon *d, struct child *k) {
	static struct wifi_net want[PROFILES_MAX];
	static struct wpa_network w;
	struct wpa_table *t = wpa_table_of(d, k->ifname);
	int disc;
	if (!t || k->wpa.ctl < 0)
		return;
	int n = core_wifi_nets(&d->core, k->ifname, want, PROFILES_MAX, &disc);
	if (disc && !t->disconnected && wpa_ok(k, "DISCONNECT")) {
		t->disconnected = 1;
		printf("networkd: %s: wpa_supplicant disconnected\n", k->ifname);
	}

	for (int j = 0; j < t->n;) {
		struct wpa_net *have = &t->items[j];
		int keep = 0;
		for (int i = 0; i < n && !keep; i++)
			if (strcmp(want[i].p->name, have->name) == 0) {
				wpactl_network(want[i].p, &w);
				keep = w.hash == have->hash;
			}
		if (keep) {
			j++;
			continue;
		}
		wpa_ok(k, "REMOVE_NETWORK %d", have->id);
		printf("networkd: %s: profile %s removed from wpa_supplicant\n",
		       k->ifname, have->name);
		*have = t->items[--t->n];
	}

	for (int i = 0; i < n; i++) {
		struct wpa_net *have = NULL;
		for (int j = 0; j < t->n && !have; j++)
			if (strcmp(t->items[j].name, want[i].p->name) == 0)
				have = &t->items[j];
		if (have) {
			if (have->priority != want[i].priority &&
			    wpa_ok(k, "SET_NETWORK %d priority %d", have->id,
			           want[i].priority))
				have->priority = want[i].priority;
			if (have->enabled != want[i].enabled &&
			    wpa_ok(k, "%s_NETWORK %d",
			           want[i].enabled ? "ENABLE" : "DISABLE", have->id)) {
				have->enabled = want[i].enabled;
				printf("networkd: %s: profile %s %s\n", k->ifname, have->name,
				       have->enabled ? "enabled" : "disabled");
			}
			continue;
		}
		wpactl_network(want[i].p, &w);
		int id = t->n < PROFILES_MAX ? wpa_add(k, &want[i], &w) : -1;
		if (id < 0)
			continue;
		have = &t->items[t->n++];
		snprintf(have->name, sizeof(have->name), "%s", want[i].p->name);
		have->id = id;
		have->hash = w.hash;
		have->priority = want[i].priority;
		have->enabled = want[i].enabled;
		printf("networkd: %s: profile %s added as network %d, priority %d, "
		       "%s\n",
		       k->ifname, have->name, id, have->priority,
		       have->enabled ? "enabled" : "disabled");
	}

	if (!disc && t->disconnected && wpa_ok(k, "RECONNECT")) {
		t->disconnected = 0;
		printf("networkd: %s: wpa_supplicant reconnecting\n", k->ifname);
	}
}

static void wpa_attach(struct daemon *d, struct child *k) {
	struct wpa_table *t = wpa_table_of(d, k->ifname);
	if (wpa_connect(WPA_DIR, k->ifname, &k->wpa) < 0) {
		printf("networkd: %s: cannot attach to wpa_supplicant: %s\n", k->ifname,
		       strerror(errno));
		kill(k->pid, SIGTERM);
		return;
	}
	watch_fd(d, k->wpa.mon, ((uint64_t)T_WPA << 32) | (uint64_t)(k - d->kids));
	printf("networkd: %s: wpa_supplicant ready\n", k->ifname);
	if (t)
		memset(t, 0, sizeof(*t));
	if (!wpa_ok(k, "SET sae_pwe 2"))
		printf("networkd: %s: wpa_supplicant rejected sae_pwe\n", k->ifname);
	wpa_sync_nets(d, k);
}

static void on_wpa_dir(struct daemon *d) {
	char names[WPA_WATCH_MAX][IF_NAMESIZE];
	int n = wpa_watch_read(d->wfd, names, WPA_WATCH_MAX);
	for (int i = 0; i < n; i++) {
		struct child *k = running_child(d, names[i], K_WPA);
		if (k && k->wpa.ctl < 0)
			wpa_attach(d, k);
	}
}

static void on_wpa_event(struct daemon *d, struct child *k) {
	char msg[WPA_EVENT_MAX];
	int n;
	while (k->wpa.mon >= 0 &&
	       (n = wpa_event_read(k->wpa.mon, msg, sizeof(msg))) > 0) {
		struct wpa_event ev;
		struct wpa_status st;
		struct actions acts = {0};
		if (wpactl_event(msg, (size_t)n, &ev) < 0)
			continue;
		if (ev.type == WPA_EV_CONNECTED) {
			printf("networkd: %s: associated with %s\n", k->ifname, ev.bssid);
			n = wpa_cmd(k, msg, sizeof(msg), "STATUS");
			if (n < 0 || wpactl_status(msg, strlen(msg), &st) < 0) {
				printf("networkd: %s: no STATUS from wpa_supplicant\n",
				       k->ifname);
				continue;
			}
			core_wifi_assoc(&d->core, k->ifname, st.id_str, st.ssid,
			                st.ssid_len, st.bssid, now_ms(), &acts);
			execute(d, &acts);
			d->dirty = 1;
		} else if (ev.type == WPA_EV_DISCONNECTED) {
			printf("networkd: %s: disassociated from %s (reason %d)\n",
			       k->ifname, ev.bssid, ev.reason);
			core_wifi_lost(&d->core, k->ifname);
			d->dirty = 1;
		}
	}
}

static void execute(struct daemon *d, struct actions *acts) {
	for (int i = 0; i < acts->n; i++) {
		const struct action *a = &acts->a[i];
		struct child *k;
		switch (a->type) {
		case A_SPAWN_DHCPC:
			spawn_dhcpc(d, a);
			d->dirty = 1;
			break;
		case A_SPAWN_SENSOR:
			spawn_sensor(d, a);
			break;
		case A_SPAWN_PROBE:
			spawn_probe(d, a);
			break;
		case A_SPAWN_WPA:
			spawn_wpa(d, a);
			break;
		case A_WPA_SYNC:
			if ((k = running_child(d, a->iface, K_WPA)))
				wpa_sync_nets(d, k);
			break;
		case A_LINK_UP:
			if (static_link_up(a) < 0)
				printf("networkd: %s: cannot bring the interface up\n",
				       a->iface);
			break;
		case A_LINK_DOWN:
			if (static_link_down(a) < 0)
				printf("networkd: %s: cannot bring the interface down\n",
				       a->iface);
			break;
		case A_STATIC_APPLY: {
			struct actions next = {0};
			int rc = static_apply(a);
			core_static_result(&d->core, a->iface, rc, now_ms(), &next);
			execute(d, &next);
			d->dirty = 1;
			break;
		}
		case A_STATIC_REMOVE:
			static_remove(a);
			d->dirty = 1;
			break;
		case A_SIGNAL:
			if ((k = running_child(d, a->iface, a->kind)))
				kill(k->pid, a->sig);
			d->dirty = 1;
			break;
		case A_ALERT:
			own_alert(d, a);
			break;
		case A_LOG:
			printf("networkd: %s\n", a->msg);
			break;
		}
	}
	acts->n = 0;
}

static int in_sysfs(const char *name, const char *entry) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s/%s", TUNDRA_NETWORKD_SYSFS, name,
	         entry);
	return access(path, F_OK) == 0;
}

static unsigned int link_flags(const struct link_ev *ev) {
	unsigned int flags = ev->up ? LINK_UP : 0;
	if (ev->arptype == ARPHRD_ETHER && in_sysfs(ev->name, "device"))
		flags |= LINK_PHYSICAL;
	if (in_sysfs(ev->name, "phy80211"))
		flags |= LINK_WIRELESS;
	return flags;
}

static int on_link(struct daemon *d) {
	struct link_ev evs[LINK_EVENTS_MAX];
	struct actions acts = {0};
	int n = link_read(d->lfd, evs, LINK_EVENTS_MAX);
	if (n < 0) {
		perror("netlink read error");
		return -1;
	}
	for (int i = 0; i < n; i++) {
		uint64_t now = now_ms();
		if (evs[i].gone)
			core_link_gone(&d->core, evs[i].name, now, &acts);
		else
			core_link(&d->core, evs[i].name, link_flags(&evs[i]), now, &acts);
		execute(d, &acts);
	}
	return 0;
}

static void on_lease(struct daemon *d) {
	struct lease_ev evs[LEASE_EVENTS_MAX];
	int n = lease_watch_read(d->ifd, evs, LEASE_EVENTS_MAX);
	if (n > 0)
		d->dirty = 1;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < CHILDREN_MAX; j++) {
			struct child *k = &d->kids[j];
			char expect[IF_NAMESIZE + PROFILE_NAME_MAX + 16];
			if (!k->used)
				continue;
			snprintf(expect, sizeof(expect), "%s-%s.lease", k->ifname,
			         k->profile);
			if (strcmp(expect, evs[i].file) == 0)
				printf("networkd: %s: lease for %s %s\n", k->ifname, k->profile,
				       evs[i].removed ? "removed" : "updated");
		}
	}
}

static void reap(struct daemon *d) {
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < CHILDREN_MAX; i++) {
			struct child *k = &d->kids[i];
			if (!k->used || k->pid != pid)
				continue;
			drain(d, k);
			k->reaped = 1;
			d->dirty = 1;
			struct actions acts = {0};
			char ifname[IF_NAMESIZE];
			struct child done = *k;
			snprintf(ifname, sizeof(ifname), "%s", k->ifname);
			wpa_detach(d, k);
			if (k->fd < 0)
				k->used = 0;
			if (done.kind == K_WPA)
				core_wpa_exit(&d->core, ifname, status, now_ms(), &acts);
			else if (done.kind == K_SENSOR)
				core_sensor_exit(&d->core, ifname, status,
				                 done.conflict_mac[0] ? done.conflict_mac
				                                      : NULL,
				                 now_ms(), &acts);
			else if (done.kind == K_PROBE)
				core_probe_exit(&d->core, ifname, status, done.probe, done.mac,
				                done.src[0] ? done.src : NULL, now_ms(), &acts);
			else
				core_child_exit(&d->core, ifname, status, now_ms(), &acts);
			execute(d, &acts);
			break;
		}
	}
}

static const struct profile *find_profile(const struct config *cfg,
                                          const char *name) {
	for (int i = 0; i < cfg->n; i++)
		if (strcmp(cfg->profiles[i].name, name) == 0)
			return &cfg->profiles[i];
	return NULL;
}

static int read_lease(const char *ifname, const char *profile, char *buf,
                      size_t size, size_t *len) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s-%s.lease", DHCPC_LEASE_DIR, ifname,
	         profile);
	return read_file(path, buf, size, len);
}

static void log_dns(const char *text) {
	char line[RESOLV_TEXT_MAX] = "";
	size_t used = 0;
	const char *p = text;
	while ((p = strstr(p, "nameserver "))) {
		p += strlen("nameserver ");
		size_t n = strcspn(p, "\n");
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %.*s",
		                         (int)n, p);
		p += n;
	}
	printf("networkd: dns:%s\n", used ? line : " none");
}

static void write_dns(struct daemon *d, struct resolv_src *src, int n) {
	char text[RESOLV_TEXT_MAX];
	if (resolv_build(src, n, text, sizeof(text)) < 0 ||
	    (d->dns_written && strcmp(text, d->dns) == 0))
		return;
	if (files_make_dirs(TUNDRA_NETWORKD_RUN_DIR, 0755) < 0 ||
	    files_write_atomic(RESOLV_PATH, text, strlen(text), 0644) < 0) {
		fprintf(stderr, "networkd: cannot write %s: %s\n", RESOLV_PATH,
		        strerror(errno));
		d->dns_written = 0;
		return;
	}
	snprintf(d->dns, sizeof(d->dns), "%s", text);
	d->dns_written = 1;
	log_dns(text);
}

static void emit(struct daemon *d, int connected, const char *ifname,
                 const struct conn_view *v) {
	char line[REPLY_MAX];
	if (proto_event(connected, ifname, v->profile, v->address, line,
	                sizeof(line)) >= 0)
		server_broadcast(&d->srv, line);
}

static void refresh(struct daemon *d) {
	struct resolv_src src[CORE_IFACES];
	int n = 0;
	d->dirty = 0;
	for (int i = 0; i < d->core.n; i++) {
		const struct core_iface *ci = &d->core.ifs[i];
		struct conn_view *v = &d->view[i];
		struct conn_view now = {0};
		struct lease_info li;
		char lease[LEASE_FILE_MAX];
		size_t len;
		int up = 0;
		if (ci->st == C_STATIC && ci->want_addr) {
			memset(&li, 0, sizeof(li));
			li.address = ci->conn.ip;
			li.prefix = ci->conn.prefix;
			li.router = ci->conn.has_gateway ? ci->conn.gateway : 0;
			up = 1;
		} else if (ci->st == C_RUNNING &&
		           read_lease(ci->name, ci->profile, lease, sizeof(lease),
		                      &len) == 0 &&
		           leaseinfo_parse(lease, len, &li) == 0) {
			up = 1;
		}
		if (up) {
			now.connected = 1;
			snprintf(now.profile, sizeof(now.profile), "%s", ci->profile);
			leaseinfo_address(&li, now.address);
			if (resolv_source(ci->name, &ci->conn,
			                  ci->has_conn_conf ? &ci->conn_conf : NULL, &li,
			                  &src[n]) == 0)
				n++;
		}
		int same = v->connected == now.connected &&
		           strcmp(v->profile, now.profile) == 0 &&
		           strcmp(v->address, now.address) == 0;
		if (!same && v->connected)
			emit(d, 0, ci->name, v);
		if (!same && now.connected)
			emit(d, 1, ci->name, &now);
		*v = now;
		struct actions acts = {0};
		core_lease(&d->core, ci->name, now.connected ? li.address : 0,
		           now.connected ? li.router : 0, now_ms(), &acts);
		execute(d, &acts);
	}
	write_dns(d, src, n);
}

static void reply_status(struct daemon *d, int client) {
	char line[REPLY_MAX], why[CONF_ERR_MAX + 16];
	for (int i = 0; i < d->core.n; i++) {
		const struct core_iface *ci = &d->core.ifs[i];
		const char *err = core_unmanaged(&d->core, ci->name);
		struct iface_view v = {
		    .ci = ci,
		    .p = ci->st == C_IDLE ? NULL : &ci->conn,
		    .connected = d->view[i].connected,
		    .address = d->view[i].connected ? d->view[i].address : NULL,
		    .kept = ci->kept,
		    .override = ci->override,
		    .off = core_wifi_off(&d->core, ci->name),
		    .ssid = ci->associated ? ci->assid : NULL,
		    .ssid_len = ci->assid_len,
		    .bssid = ci->associated && ci->bssid[0] ? ci->bssid : NULL,
		};
		char ids[REPLY_MAX];
		if (alerts_ids_on(&d->alerts, ci->name, ids, sizeof(ids)) == 0)
			v.alerts = ids;
		if (err) {
			snprintf(why, sizeof(why), "interfaces: %s", err);
			v.unmanaged = why;
		}
		if (ci->present || ci->st == C_WAIT_LINK)
			if (proto_status(&v, line, sizeof(line)) >= 0)
				server_send(&d->srv, client, line);
	}
	server_send(&d->srv, client, "OK");
}

static void reply_profiles(struct daemon *d, int client) {
	char line[REPLY_MAX];
	for (int i = 0; i < d->cfg->n; i++)
		if (proto_profile(&d->cfg->profiles[i], line, sizeof(line)) >= 0)
			server_send(&d->srv, client, line);
	server_send(&d->srv, client, "OK");
}

static const char *do_connect(struct daemon *d, const struct cmd *cmd) {
	const struct profile *p = find_profile(d->cfg, cmd->args[0]);
	const char *ifname = cmd->nargs > 1 ? cmd->args[1] : NULL;
	struct actions acts = {0};
	if (p && p->valid && !ifname) {
		if (p->any_interface)
			return "ERROR interface required";
		ifname = p->interface;
	}
	enum connect_result r = core_connect(&d->core, cmd->args[0],
	                                     ifname ? ifname : "", now_ms(), &acts);
	execute(d, &acts);
	return proto_connect_error(r);
}

static void report_config(const struct config *cfg);

static int do_reload(struct daemon *d) {
	struct actions acts = {0};
	if (conf_load(TUNDRA_NETWORKD_CONF_DIR, d->spare) < 0) {
		printf("networkd: reload failed, keeping the configuration\n");
		return -1;
	}
	report_config(d->spare);
	core_reload(&d->core, d->spare, now_ms(), &acts);
	struct config *old = d->cfg;
	d->cfg = d->spare;
	d->spare = old;
	execute(d, &acts);
	d->dirty = 1;
	printf("networkd: configuration reloaded\n");
	return 0;
}

static void reply_alerts(struct daemon *d, int client) {
	char line[REPLY_MAX];
	for (int i = 0; i < d->alerts.n; i++)
		if (alerts_format(&d->alerts.items[i], line, sizeof(line)) >= 0)
			server_send(&d->srv, client, line);
	server_send(&d->srv, client, "OK");
}

static const char *do_reply(struct daemon *d, const struct cmd *cmd) {
	char *end;
	unsigned long id = strtoul(cmd->args[0], &end, 10);
	const struct stored_alert *sa = *end || !id || id > 0xffffffffUL
	                                    ? NULL
	                                    : alerts_find(&d->alerts, (unsigned)id);
	const char *opt = cmd->args[1];
	struct actions acts = {0};
	if (!sa)
		return "ERROR unknown alert";
	if (!alerts_offers(sa, opt))
		return "ERROR option not offered by that alert";

	struct stored_alert copy = *sa;
	uint64_t now = now_ms();
	if (copy.a.action == BLUEICE_STOP && strcmp(opt, "force") == 0) {
		core_force(&d->core, copy.id, copy.a.on, copy.a.kase, copy.a.level, now,
		           &acts);
	} else if (copy.a.action == BLUEICE_STOP &&
	           strcmp(opt, "disconnect") == 0) {
		core_release_alert(&d->core, copy.id, now, &acts);
	} else if (strcmp(opt, "disconnect") == 0) {
		const struct core_iface *ci = core_find(&d->core, copy.a.on);
		if (ci && ci->st != C_IDLE && strcmp(ci->profile, copy.profile) == 0)
			core_disconnect(&d->core, copy.a.on, now, &acts);
	}
	alerts_remove(&d->alerts, copy.id);
	save_alerts(d);
	printf("networkd: alert %u answered with %s\n", copy.id, opt);
	alert_closed(d, copy.id, opt);
	execute(d, &acts);
	d->dirty = 1;
	return NULL;
}

static void on_command(void *ctx, int client, const char *line) {
	struct daemon *d = ctx;
	struct actions acts = {0};
	struct cmd cmd;
	const char *err;
	proto_parse(line, &cmd);
	switch (cmd.type) {
	case CMD_EMPTY:
		return;
	case CMD_BAD:
		server_send(&d->srv, client, cmd.error);
		return;
	case CMD_STATUS:
		reply_status(d, client);
		return;
	case CMD_PROFILES:
		reply_profiles(d, client);
		return;
	case CMD_CONNECT:
		err = do_connect(d, &cmd);
		server_send(&d->srv, client, err ? err : "OK");
		return;
	case CMD_DISCONNECT:
		if (core_disconnect(&d->core, cmd.args[0], now_ms(), &acts) < 0) {
			server_send(&d->srv, client, "ERROR no such interface");
			return;
		}
		execute(d, &acts);
		server_send(&d->srv, client, "OK");
		return;
	case CMD_RELOAD:
		server_send(&d->srv, client,
		            do_reload(d) < 0 ? "ERROR cannot read the configuration"
		                             : "OK");
		return;
	case CMD_SUBSCRIBE:
		d->srv.c[client].subscribed = 1;
		server_send(&d->srv, client, "OK");
		return;
	case CMD_ALERTS:
		reply_alerts(d, client);
		return;
	case CMD_ALERT_REPLY:
		err = do_reply(d, &cmd);
		server_send(&d->srv, client, err ? err : "OK");
		return;
	}
}

static int on_signal(struct daemon *d) {
	struct signalfd_siginfo si;
	struct actions acts = {0};
	while (read(d->sfd, &si, sizeof(si)) == sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGCHLD:
			reap(d);
			break;
		case SIGHUP:
			do_reload(d);
			break;
		default:
			printf("networkd: received %s, stopping\n",
			       strsignal((int)si.ssi_signo));
			core_shutdown(&d->core, now_ms(), &acts);
			execute(d, &acts);
			break;
		}
	}
	return 0;
}

static int open_signals(void) {
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);
	/* An inherited SIG_IGN discards the signal before signalfd sees it, and
	 * for SIGCHLD it reaps the children behind waitpid's back. */
	for (int sig = 1; sig < NSIG; sig++)
		if (sigismember(&mask, sig) == 1)
			signal(sig, SIG_DFL);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("sigprocmask error");
		return -1;
	}
	int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (fd < 0)
		perror("signalfd error");
	return fd;
}

static void report_config(const struct config *cfg) {
	if (!cfg->ifaces.valid)
		printf("networkd: interfaces: %s\n", cfg->ifaces.error);
	for (int i = 0; cfg->ifaces.valid && i < cfg->ifaces.n; i++)
		if (!cfg->ifaces.items[i].valid)
			printf("networkd: interfaces: %s: %s\n", cfg->ifaces.items[i].name,
			       cfg->ifaces.items[i].error);
	for (int i = 0; i < cfg->n; i++)
		if (!cfg->profiles[i].valid)
			printf("networkd: profile %s: %s\n", cfg->profiles[i].name,
			       cfg->profiles[i].error);
	if (cfg->truncated)
		printf("networkd: more than %d profiles, the rest are ignored\n",
		       PROFILES_MAX);
	printf("networkd: profiles loaded: %d\n", cfg->n);
}

int loop_run(struct config *cfg) {
	static struct daemon d;
	static struct config spare;
	memset(&d, 0, sizeof(d));
	d.cfg = cfg;
	d.spare = &spare;
	for (int i = 0; i < CHILDREN_MAX; i++)
		d.kids[i].fd = d.kids[i].wpa.ctl = d.kids[i].wpa.mon = -1;
	core_init(&d.core, cfg);
	load_alerts(&d);
	d.dirty = 1;
	report_config(cfg);

	d.ep = epoll_create1(EPOLL_CLOEXEC);
	d.sfd = open_signals();
	d.lfd = link_open();
	d.ifd = lease_watch_open(DHCPC_LEASE_DIR);
	d.wfd = wpa_watch_open(WPA_DIR);
	if (d.ep < 0 || d.sfd < 0 || d.lfd < 0 || d.ifd < 0 || d.wfd < 0 ||
	    watch_fd(&d, d.sfd, (uint64_t)T_SIGNAL << 32) < 0 ||
	    watch_fd(&d, d.lfd, (uint64_t)T_LINK << 32) < 0 ||
	    watch_fd(&d, d.ifd, (uint64_t)T_LEASE << 32) < 0 ||
	    watch_fd(&d, d.wfd, (uint64_t)T_WPA_DIR << 32) < 0 ||
	    server_open(&d.srv, TUNDRA_NETWORKD_SOCKET, d.ep,
	                (uint64_t)T_LISTEN << 32, (uint64_t)T_CLIENT << 32) < 0 ||
	    link_dump(d.lfd) < 0) {
		server_close(&d.srv);
		return 1;
	}

	for (;;) {
		if (core_done(&d.core)) {
			server_close(&d.srv);
			return 0;
		}

		uint64_t now = now_ms();
		uint64_t next = core_next_deadline(&d.core);
		int timeout = -1;
		if (next != UINT64_MAX)
			timeout = next > now ? (int)(next - now) : 0;

		struct epoll_event evs[16];
		int n = epoll_wait(d.ep, evs, 16, timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait error");
			server_close(&d.srv);
			return 1;
		}

		struct actions acts = {0};
		core_timer(&d.core, now_ms(), &acts);
		execute(&d, &acts);

		for (int i = 0; i < n; i++) {
			uint32_t type = (uint32_t)(evs[i].data.u64 >> 32);
			uint32_t idx = (uint32_t)evs[i].data.u64;
			if (type == T_SIGNAL)
				on_signal(&d);
			else if (type == T_LINK && on_link(&d) < 0) {
				server_close(&d.srv);
				return 1;
			} else if (type == T_LEASE)
				on_lease(&d);
			else if (type == T_CHILD && idx < CHILDREN_MAX && d.kids[idx].used)
				drain(&d, &d.kids[idx]);
			else if (type == T_WPA_DIR)
				on_wpa_dir(&d);
			else if (type == T_WPA && idx < CHILDREN_MAX && d.kids[idx].used)
				on_wpa_event(&d, &d.kids[idx]);
			else if (type == T_LISTEN)
				server_accept(&d.srv);
			else if (type == T_CLIENT && idx < SERVER_CLIENTS) {
				if (evs[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
					server_output(&d.srv, (int)idx);
				if (evs[i].events & EPOLLIN)
					server_input(&d.srv, (int)idx, on_command, &d);
				d.dirty = 1;
			}
		}
		if (d.dirty && !d.core.shutting_down)
			refresh(&d);
	}
}
