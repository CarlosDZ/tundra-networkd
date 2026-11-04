#include "loop.h"
#include "core.h"
#include "files.h"
#include "lease.h"
#include "link.h"
#include "resolv.h"
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_arp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILDREN_MAX (CORE_IFACES * 2)
#define OUT_LINE_MAX 512
#define DHCPC_LEASE_DIR "/run/tundra/dhcpc"
#define LEASE_FILE_MAX 1024
#define RESOLV_PATH TUNDRA_NETWORKD_RUN_DIR "/resolv.conf"
#define T_SIGNAL 1
#define T_LINK 2
#define T_LEASE 3
#define T_CHILD 4

struct child {
	int used;
	pid_t pid;
	int fd;
	int reaped;
	char ifname[IF_NAMESIZE];
	char profile[PROFILE_NAME_MAX + 1];
	char line[OUT_LINE_MAX];
	size_t len;
};

struct daemon {
	struct config *cfg;
	struct core core;
	int ep;
	int sfd;
	int lfd;
	int ifd;
	struct child kids[CHILDREN_MAX];
	int dns_dirty;
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

static void emit_line(struct child *k) {
	k->line[k->len] = '\0';
	printf("dhcpc[%s/%s]: %s\n", k->ifname, k->profile, k->line);
	k->len = 0;
}

static void release_child(struct daemon *d, struct child *k) {
	if (k->fd >= 0) {
		if (k->len)
			emit_line(k);
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
				emit_line(k);
				if (buf[i] == '\n')
					continue;
			}
			k->line[k->len++] = buf[i];
		}
	}
}

static struct child *running_child(struct daemon *d, const char *ifname) {
	for (int i = 0; i < CHILDREN_MAX; i++) {
		struct child *k = &d->kids[i];
		if (k->used && !k->reaped && strcmp(k->ifname, ifname) == 0)
			return k;
	}
	return NULL;
}

static void execute(struct daemon *d, struct actions *acts);

static void spawn_dhcpc(struct daemon *d, const struct action *a) {
	char metric[16], grace[16];
	char *argv[16];
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
	argv[n++] = "lease-key";
	argv[n++] = (char *)a->profile;
	argv[n] = NULL;

	struct child *k = NULL;
	for (int i = 0; i < CHILDREN_MAX && !k; i++)
		if (!d->kids[i].used)
			k = &d->kids[i];

	int fd = -1;
	pid_t pid = k ? spawn_child(TUNDRA_DHCPC_PATH, argv, &fd) : -1;
	if (pid < 0) {
		struct actions acts = {0};
		fprintf(stderr, "networkd: %s: cannot launch tundra-dhcpc\n", a->iface);
		core_child_exit(&d->core, a->iface, 1 << 8, now_ms(), &acts);
		execute(d, &acts);
		return;
	}

	memset(k, 0, sizeof(*k));
	k->used = 1;
	k->pid = pid;
	k->fd = fd;
	snprintf(k->ifname, sizeof(k->ifname), "%s", a->iface);
	snprintf(k->profile, sizeof(k->profile), "%s", a->profile);
	watch_fd(d, fd, ((uint64_t)T_CHILD << 32) | (uint64_t)(k - d->kids));
	printf("networkd: %s: tundra-dhcpc started for profile %s (pid %d)\n",
	       a->iface, a->profile, (int)pid);
}

static void execute(struct daemon *d, struct actions *acts) {
	for (int i = 0; i < acts->n; i++) {
		const struct action *a = &acts->a[i];
		struct child *k;
		switch (a->type) {
		case A_SPAWN_DHCPC:
			spawn_dhcpc(d, a);
			d->dns_dirty = 1;
			break;
		case A_SIGNAL:
			if ((k = running_child(d, a->iface)))
				kill(k->pid, a->sig);
			d->dns_dirty = 1;
			break;
		case A_LOG:
			printf("networkd: %s\n", a->msg);
			break;
		}
	}
	acts->n = 0;
}

static int is_physical(const char *name, unsigned short arptype) {
	char path[256];
	if (arptype != ARPHRD_ETHER)
		return 0;
	snprintf(path, sizeof(path), "%s/%s/device", TUNDRA_NETWORKD_SYSFS, name);
	return access(path, F_OK) == 0;
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
			core_link(&d->core, evs[i].name,
			          is_physical(evs[i].name, evs[i].arptype), now, &acts);
		execute(d, &acts);
	}
	return 0;
}

static void on_lease(struct daemon *d) {
	struct lease_ev evs[LEASE_EVENTS_MAX];
	int n = lease_watch_read(d->ifd, evs, LEASE_EVENTS_MAX);
	if (n > 0)
		d->dns_dirty = 1;
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
			d->dns_dirty = 1;
			struct actions acts = {0};
			char ifname[IF_NAMESIZE];
			snprintf(ifname, sizeof(ifname), "%s", k->ifname);
			if (k->fd < 0)
				k->used = 0;
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

static void update_dns(struct daemon *d) {
	struct resolv_src src[CORE_IFACES];
	int n = 0;
	d->dns_dirty = 0;
	for (int i = 0; i < d->core.n; i++) {
		const struct core_iface *ci = &d->core.ifs[i];
		char lease[LEASE_FILE_MAX];
		size_t len;
		if (ci->st != C_RUNNING ||
		    read_lease(ci->name, ci->profile, lease, sizeof(lease), &len) < 0)
			continue;
		if (resolv_source(ci->name, find_profile(d->cfg, ci->profile),
		                  ifaces_find(&d->cfg->ifaces, ci->name), lease, len,
		                  &src[n]) == 0)
			n++;
	}

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

static int on_signal(struct daemon *d) {
	struct signalfd_siginfo si;
	struct actions acts = {0};
	while (read(d->sfd, &si, sizeof(si)) == sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGCHLD:
			reap(d);
			break;
		case SIGHUP:
			printf("networkd: reload is not implemented yet\n");
			break;
		default:
			printf("networkd: received %s, stopping\n",
			       strsignal((int)si.ssi_signo));
			core_shutdown(&d->core, &acts);
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
	memset(&d, 0, sizeof(d));
	d.cfg = cfg;
	for (int i = 0; i < CHILDREN_MAX; i++)
		d.kids[i].fd = -1;
	core_init(&d.core, cfg);
	d.dns_dirty = 1;
	report_config(cfg);

	d.ep = epoll_create1(EPOLL_CLOEXEC);
	d.sfd = open_signals();
	d.lfd = link_open();
	d.ifd = lease_watch_open(DHCPC_LEASE_DIR);
	if (d.ep < 0 || d.sfd < 0 || d.lfd < 0 || d.ifd < 0 ||
	    watch_fd(&d, d.sfd, (uint64_t)T_SIGNAL << 32) < 0 ||
	    watch_fd(&d, d.lfd, (uint64_t)T_LINK << 32) < 0 ||
	    watch_fd(&d, d.ifd, (uint64_t)T_LEASE << 32) < 0 ||
	    link_dump(d.lfd) < 0)
		return 1;

	for (;;) {
		if (core_done(&d.core))
			return 0;

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
			else if (type == T_LINK && on_link(&d) < 0)
				return 1;
			else if (type == T_LEASE)
				on_lease(&d);
			else if (type == T_CHILD && idx < CHILDREN_MAX && d.kids[idx].used)
				drain(&d, &d.kids[idx]);
		}
		if (d.dns_dirty && !d.core.shutting_down)
			update_dns(&d);
	}
}
