#include "lease.h"
#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

int lease_watch_open(const char *dir) {
	if (files_make_dirs(dir, 0700) < 0) {
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
