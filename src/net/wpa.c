#include "wpa.h"
#include "files.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int wpa_watch_open(const char *dir) {
	if (files_make_dirs(dir, 0700) < 0) {
		fprintf(stderr, "cannot create %s: %s\n", dir, strerror(errno));
		return -1;
	}
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		perror("inotify error");
		return -1;
	}
	if (inotify_add_watch(fd, dir, IN_CREATE) < 0) {
		fprintf(stderr, "cannot watch %s: %s\n", dir, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int wpa_watch_read(int fd, char names[][IF_NAMESIZE], int max) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len = read(fd, buf, sizeof(buf));
	if (len < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;

	int n = 0;
	for (char *p = buf; p < buf + len;) {
		struct inotify_event *ie = (struct inotify_event *)p;
		p += sizeof(*ie) + ie->len;
		if (!ie->len || strlen(ie->name) >= IF_NAMESIZE || n == max)
			continue;
		snprintf(names[n++], IF_NAMESIZE, "%s", ie->name);
	}
	return n;
}

static int open_sock(const char *path) {
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	/* Autobind to an abstract name: wpa_supplicant answers to it and there is
	 * no file to clean up. */
	struct sockaddr_un local = {.sun_family = AF_UNIX};
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (bind(fd, (struct sockaddr *)&local, sizeof(sa_family_t)) < 0 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

int wpa_request(int fd, const char *cmd, char *reply, size_t size) {
	if (send(fd, cmd, strlen(cmd), 0) < 0)
		return -1;
	struct pollfd p = {.fd = fd, .events = POLLIN};
	for (;;) {
		int r = poll(&p, 1, WPA_TIMEOUT_MS);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = ETIMEDOUT;
			return -1;
		}
		ssize_t n = recv(fd, reply, size - 1, 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n < 0)
			return -1;
		reply[n] = '\0';
		if (n > 0 && reply[0] == '<')
			continue;
		return (int)n;
	}
}

static int expect(int fd, const char *cmd, const char *want) {
	char reply[64];
	if (wpa_request(fd, cmd, reply, sizeof(reply)) < 0)
		return -1;
	if (strcmp(reply, want) != 0) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

int wpa_connect(const char *dir, const char *ifname, struct wpa_conn *w) {
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	w->ctl = w->mon = -1;
	if (snprintf(path, sizeof(path), "%s/%s", dir, ifname) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if ((w->ctl = open_sock(path)) < 0 || (w->mon = open_sock(path)) < 0 ||
	    expect(w->ctl, "PING", "PONG\n") < 0 ||
	    expect(w->mon, "ATTACH", "OK\n") < 0) {
		int saved = errno;
		wpa_close(w);
		errno = saved;
		return -1;
	}
	return 0;
}

int wpa_event_read(int fd, char *buf, size_t size) {
	ssize_t n = recv(fd, buf, size - 1, 0);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	buf[n] = '\0';
	return (int)n;
}

void wpa_close(struct wpa_conn *w) {
	if (w->ctl >= 0)
		close(w->ctl);
	if (w->mon >= 0)
		close(w->mon);
	w->ctl = w->mon = -1;
}
