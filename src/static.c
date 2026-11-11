#include "static.h"
#include "hwaddr.h"
#include "spawn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIP_TIMEOUT_MS 5000
#define TIP_ARGS_MAX 16
#define RULE_PRIORITY "1000"

static void print_output(const char *ifname, int fd) {
	char out[2048];
	size_t len = 0;
	ssize_t n;
	while (len < sizeof(out) - 1 &&
	       (n = read(fd, out + len, sizeof(out) - 1 - len)) > 0)
		len += (size_t)n;
	out[len] = '\0';
	for (char *line = out, *nl; *line; line = nl + 1) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		printf("tundra-ip[%s]: %s\n", ifname, line);
		if (!nl)
			break;
	}
}

/* A NULL keyword ends the list early, so an optional pair always goes last. */
static int tip(const char *ifname, int quiet, ...) {
	char *argv[TIP_ARGS_MAX + 2];
	int argc = 0;
	va_list ap;
	va_start(ap, quiet);
	argv[argc++] = "tundra-ip";
	for (const char *s; argc <= TIP_ARGS_MAX && (s = va_arg(ap, const char *));)
		argv[argc++] = (char *)s;
	va_end(ap);
	argv[argc] = NULL;

	int fd;
	pid_t pid = spawn_child(TUNDRA_IP_PATH, argv, &fd);
	if (pid < 0)
		return STATIC_FAIL;
	int status = 0;
	struct timespec tick = {0, 10000000};
	for (int waited = 0;; waited += 10) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			break;
		if (r < 0 && errno != EINTR) {
			close(fd);
			return STATIC_FAIL;
		}
		if (waited >= TIP_TIMEOUT_MS) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			fprintf(stderr, "networkd: %s: tundra-ip did not finish, killed\n",
			        ifname);
			break;
		}
		nanosleep(&tick, NULL);
	}
	if (!quiet)
		print_output(ifname, fd);
	close(fd);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 2 ? STATIC_USAGE
	                                                     : STATIC_FAIL;
}

struct addrs {
	char ip[INET_ADDRSTRLEN];
	char cidr[INET_ADDRSTRLEN + 3];
	char net[INET_ADDRSTRLEN + 3];
	char host[INET_ADDRSTRLEN + 3];
	char gw[INET_ADDRSTRLEN];
	char metric[16];
};

static void format(const struct action *a, struct addrs *s) {
	char net[INET_ADDRSTRLEN];
	uint32_t mask = a->prefix ? htonl(0xffffffffu << (32 - a->prefix)) : 0;
	uint32_t netaddr = a->addr & mask;
	inet_ntop(AF_INET, &a->addr, s->ip, sizeof(s->ip));
	inet_ntop(AF_INET, &netaddr, net, sizeof(net));
	inet_ntop(AF_INET, &a->gw, s->gw, sizeof(s->gw));
	snprintf(s->cidr, sizeof(s->cidr), "%s/%d", s->ip, a->prefix);
	snprintf(s->net, sizeof(s->net), "%s/%d", net, a->prefix);
	snprintf(s->host, sizeof(s->host), "%s/32", s->ip);
	snprintf(s->metric, sizeof(s->metric), "%u", a->metric);
}

static int default_replace(const struct action *a, const struct addrs *s,
                           const char *table) {
	const char *metric_kw = a->has_metric ? "metric" : NULL;
	if (table)
		return tip(a->iface, 0, "route", "replace", "default", "via", s->gw,
		           "on", a->iface, "table", table, metric_kw, s->metric, NULL);
	return tip(a->iface, 0, "route", "replace", "default", "via", s->gw, "on",
	           a->iface, metric_kw, s->metric, NULL);
}

int static_remove(const struct action *a) {
	struct addrs s;
	format(a, &s);
	if (a->has_table) {
		tip(a->iface, 1, "rule", "del", "from", s.host, "table", a->table,
		    "priority", RULE_PRIORITY, NULL);
		tip(a->iface, 1, "route", "del", "default", "on", a->iface, "table",
		    a->table, NULL);
		tip(a->iface, 1, "route", "del", s.net, "on", a->iface, "table",
		    a->table, NULL);
	}
	if (a->gw)
		tip(a->iface, 1, "route", "del", "default", "on", a->iface, NULL);
	return tip(a->iface, 0, "addr", "del", s.cidr, "on", a->iface, NULL);
}

static int apply_all(const struct action *a, const struct addrs *s) {
	int r;
	const char *metric_kw = a->has_metric ? "metric" : NULL;
	if ((r = tip(a->iface, 0, "addr", "replace", s->cidr, "on", a->iface,
	             metric_kw, s->metric, NULL)) < 0)
		return r;
	if (a->has_table && (r = tip(a->iface, 0, "route", "replace", s->net, "on",
	                             a->iface, "table", a->table, "src", s->ip,
	                             metric_kw, s->metric, NULL)) < 0)
		return r;
	if (a->gw && (r = default_replace(a, s, NULL)) < 0)
		return r;
	if (a->gw && a->has_table && (r = default_replace(a, s, a->table)) < 0)
		return r;
	if (a->has_table) {
		tip(a->iface, 1, "rule", "del", "from", s->host, "table", a->table,
		    "priority", RULE_PRIORITY, NULL);
		if ((r = tip(a->iface, 0, "rule", "add", "from", s->host, "table",
		             a->table, "priority", RULE_PRIORITY, NULL)) < 0)
			return r;
	}
	return 0;
}

int static_apply(const struct action *a) {
	struct addrs s;
	format(a, &s);
	int r;
	if (a->has_table &&
	    (r = tip(a->iface, 1, "route", "show", "table", a->table, NULL)) < 0)
		return r;
	if ((r = apply_all(a, &s)) < 0)
		static_remove(a);
	return r;
}

/* 0 when the interface ends with the address asked for. keep on an interface
 * without a hardware address leaves the one it has. */
int static_set_mac(const struct action *a) {
	unsigned char want[HWADDR_LEN], now[HWADDR_LEN];
	char text[HWADDR_TEXT];
	switch (a->mac.mode) {
	case MAC_FIXED:
		memcpy(want, a->mac.addr, HWADDR_LEN);
		break;
	case MAC_RANDOM:
		if (hwaddr_random(want) < 0)
			return STATIC_FAIL;
		break;
	case MAC_KEEP:
	case MAC_UNSET:
		if (hwaddr_permanent(a->iface, want) < 0)
			return 0;
		break;
	}
	if (hwaddr_current(a->iface, now) == 0 &&
	    memcmp(now, want, HWADDR_LEN) == 0)
		return 0;
	hwaddr_format(want, text);
	int up = hwaddr_is_up(a->iface) > 0;
	int r;
	if (up && (r = tip(a->iface, 0, "link", "down", a->iface, NULL)) < 0)
		return r;
	r = tip(a->iface, 0, "link", "set", "mac", text, "on", a->iface, NULL);
	if (up && tip(a->iface, 0, "link", "up", a->iface, NULL) < 0 && r == 0)
		r = STATIC_FAIL;
	if (r == 0)
		printf("networkd: %s: mac set to %s\n", a->iface, text);
	return r;
}

int static_link_up(const struct action *a) {
	return tip(a->iface, 0, "link", "up", a->iface, NULL);
}

int static_link_down(const struct action *a) {
	return tip(a->iface, 0, "link", "down", a->iface, NULL);
}
