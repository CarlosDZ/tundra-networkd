#include "server.h"
#include "files.h"

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int set_events(struct server *s, int idx, uint32_t events) {
	struct epoll_event ev = {.events = events,
	                         .data.u64 = s->tag_client | (uint64_t)idx};
	return epoll_ctl(s->ep, EPOLL_CTL_MOD, s->c[idx].fd, &ev);
}

static void drop(struct server *s, int idx) {
	struct client *c = &s->c[idx];
	if (c->fd < 0)
		return;
	epoll_ctl(s->ep, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
}

static int make_parent(const char *path) {
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;
	*slash = '\0';
	return files_make_dirs(dir, 0755);
}

static int someone_listening(const struct sockaddr_un *sa) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	int up = connect(fd, (const struct sockaddr *)sa, sizeof(*sa)) == 0;
	close(fd);
	return up;
}

static void set_owner(const char *path) {
	struct group *g = getgrnam("network");
	if (g && chown(path, (uid_t)-1, g->gr_gid) == 0 && chmod(path, 0660) == 0)
		return;
	chmod(path, 0600);
	if (g)
		printf("networkd: cannot give the control socket to group network: "
		       "%s, limited to root\n",
		       strerror(errno));
	else
		printf("networkd: group network not found, control socket limited to "
		       "root\n");
}

int server_open(struct server *s, const char *path, int ep, uint64_t tag_listen,
                uint64_t tag_client) {
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	memset(s, 0, sizeof(*s));
	s->ep = ep;
	s->fd = -1;
	s->tag_client = tag_client;
	for (int i = 0; i < SERVER_CLIENTS; i++)
		s->c[i].fd = -1;
	if (strlen(path) >= sizeof(sa.sun_path) ||
	    strlen(path) >= sizeof(s->path)) {
		fprintf(stderr, "networkd: socket path too long: %s\n", path);
		return -1;
	}
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (make_parent(path) < 0) {
		fprintf(stderr, "networkd: cannot create the directory of %s: %s\n",
		        path, strerror(errno));
		return -1;
	}
	if (someone_listening(&sa)) {
		fprintf(stderr, "networkd: another tundra-networkd answers on %s\n",
		        path);
		return -1;
	}
	if (unlink(path) < 0 && errno != ENOENT) {
		perror("unlink error");
		return -1;
	}

	s->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s->fd < 0) {
		perror("socket error");
		return -1;
	}
	mode_t old = umask(077);
	int bound = bind(s->fd, (struct sockaddr *)&sa, sizeof(sa));
	umask(old);
	if (bound < 0 || listen(s->fd, SERVER_CLIENTS) < 0) {
		perror("bind error");
		close(s->fd);
		s->fd = -1;
		return -1;
	}
	snprintf(s->path, sizeof(s->path), "%s", path);
	set_owner(path);

	struct epoll_event ev = {.events = EPOLLIN, .data.u64 = tag_listen};
	if (epoll_ctl(ep, EPOLL_CTL_ADD, s->fd, &ev) < 0) {
		perror("epoll_ctl error");
		return -1;
	}
	printf("networkd: control socket on %s\n", path);
	return 0;
}

void server_accept(struct server *s) {
	for (;;) {
		int fd = accept4(s->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			return;
		int idx = -1;
		for (int i = 0; i < SERVER_CLIENTS && idx < 0; i++)
			if (s->c[i].fd < 0)
				idx = i;
		if (idx < 0) {
			static const char full[] = "ERROR too many clients\n";
			ssize_t ignored =
			    send(fd, full, sizeof(full) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
			(void)ignored;
			close(fd);
			continue;
		}
		struct client *c = &s->c[idx];
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		c->events = EPOLLIN;
		struct epoll_event ev = {.events = EPOLLIN,
		                         .data.u64 = s->tag_client | (uint64_t)idx};
		if (epoll_ctl(s->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
			close(fd);
			c->fd = -1;
		}
	}
}

void server_output(struct server *s, int idx) {
	struct client *c = &s->c[idx];
	while (c->fd >= 0 && c->outlen) {
		ssize_t n = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n <= 0) {
			drop(s, idx);
			return;
		}
		memmove(c->out, c->out + n, c->outlen - (size_t)n);
		c->outlen -= (size_t)n;
	}
	if (c->fd < 0)
		return;
	if (!c->outlen && c->closing) {
		drop(s, idx);
		return;
	}
	uint32_t want = (c->closing ? 0 : EPOLLIN) | (c->outlen ? EPOLLOUT : 0);
	if (want != c->events && set_events(s, idx, want) == 0)
		c->events = want;
}

void server_send(struct server *s, int idx, const char *line) {
	struct client *c = &s->c[idx];
	size_t len = strlen(line);
	if (c->fd < 0)
		return;
	if (c->outlen + len + 1 > sizeof(c->out)) {
		drop(s, idx);
		return;
	}
	memcpy(c->out + c->outlen, line, len);
	c->out[c->outlen + len] = '\n';
	c->outlen += len + 1;
	server_output(s, idx);
}

void server_broadcast(struct server *s, const char *line) {
	for (int i = 0; i < SERVER_CLIENTS; i++)
		if (s->c[i].fd >= 0 && s->c[i].subscribed && !s->c[i].closing)
			server_send(s, i, line);
}

void server_input(struct server *s, int idx, server_handler h, void *ctx) {
	struct client *c = &s->c[idx];
	while (c->fd >= 0 && !c->closing) {
		ssize_t n = read(c->fd, c->in + c->inlen, sizeof(c->in) - c->inlen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			c->closing = 1;
			server_output(s, idx);
			return;
		}
		c->inlen += (size_t)n;

		char *start = c->in, *nl;
		while (c->fd >= 0 && !c->closing &&
		       (nl = memchr(start, '\n', c->inlen - (size_t)(start - c->in)))) {
			*nl = '\0';
			if (nl > start && nl[-1] == '\r')
				nl[-1] = '\0';
			h(ctx, idx, start);
			start = nl + 1;
		}
		if (c->fd < 0)
			return;
		size_t rest = c->inlen - (size_t)(start - c->in);
		memmove(c->in, start, rest);
		c->inlen = rest;
		if (c->inlen == sizeof(c->in)) {
			server_send(s, idx, "ERROR line too long");
			c->closing = 1;
			server_output(s, idx);
			return;
		}
	}
}

void server_close(struct server *s) {
	for (int i = 0; i < SERVER_CLIENTS; i++)
		drop(s, i);
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
		unlink(s->path);
	}
}
