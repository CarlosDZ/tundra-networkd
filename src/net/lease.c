#include "lease.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

static int make_dirs(const char *dir) {
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s", dir) >= (int)sizeof(path))
		return -1;
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(path, 0700) < 0 && errno != EEXIST ? -1 : 0;
}

int lease_watch_open(const char *dir) {
	if (make_dirs(dir) < 0) {
		fprintf(stderr, "cannot create %s: %s\n", dir, strerror(errno));
		return -1;
	}
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		perror("inotify error");
		return -1;
	}
	if (inotify_add_watch(fd, dir, IN_MOVED_TO | IN_DELETE) < 0) {
		fprintf(stderr, "cannot watch %s: %s\n", dir, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int is_lease(const char *name) {
	size_t n = strlen(name);
	return n > 6 && strcmp(name + n - 6, ".lease") == 0;
}

int lease_watch_read(int fd, struct lease_ev *evs, int max) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len = read(fd, buf, sizeof(buf));
	if (len < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;

	int n = 0;
	for (char *p = buf; p < buf + len;) {
		struct inotify_event *ie = (struct inotify_event *)p;
		p += sizeof(*ie) + ie->len;
		if (!ie->len || !is_lease(ie->name) || n == max)
			continue;
		evs[n].removed = (ie->mask & IN_DELETE) != 0;
		snprintf(evs[n].file, sizeof(evs[n].file), "%s", ie->name);
		n++;
	}
	return n;
}
