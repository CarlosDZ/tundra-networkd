#include "load.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEMPLATE "000-template.tnc"

static int read_file(int dirfd, const char *file, char *buf, size_t size,
                     size_t *len, struct stat *st, char err[CONF_ERR_MAX]) {
	int fd = openat(dirfd, file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		snprintf(err, CONF_ERR_MAX, "cannot open: %s", strerror(errno));
		return -1;
	}
	if (fstat(fd, st) < 0 || !S_ISREG(st->st_mode)) {
		snprintf(err, CONF_ERR_MAX, "not a regular file");
		close(fd);
		return -1;
	}
	size_t n = 0;
	for (;;) {
		ssize_t r = read(fd, buf + n, size - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			snprintf(err, CONF_ERR_MAX, "cannot read: %s", strerror(errno));
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		n += (size_t)r;
		if (n == size) {
			snprintf(err, CONF_ERR_MAX, "file larger than %zu bytes", size - 1);
			close(fd);
			return -1;
		}
	}
	close(fd);
	*len = n;
	return 0;
}

static void load_ifaces(int dirfd, struct ifaces *f) {
	static char buf[CONF_FILE_MAX];
	char err[CONF_ERR_MAX];
	struct stat st;
	size_t len;

	if (fstatat(dirfd, "interfaces", &st, AT_SYMLINK_NOFOLLOW) < 0 &&
	    errno == ENOENT) {
		memset(f, 0, sizeof(*f));
		f->valid = 1;
		return;
	}
	if (read_file(dirfd, "interfaces", buf, sizeof(buf), &len, &st, err) < 0) {
		memset(f, 0, sizeof(*f));
		snprintf(f->error, sizeof(f->error), "%s", err);
		return;
	}
	ifaces_parse(f, buf, len);
}

static int by_name(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_profile(int dirfd, const char *file, struct profile *p) {
	static char buf[CONF_FILE_MAX];
	char name[PROFILE_NAME_MAX + 1];
	char err[CONF_ERR_MAX];
	struct stat st;
	size_t len;

	memset(p, 0, sizeof(*p));
	if (profile_name_from_file(file, name) < 0) {
		snprintf(p->name, sizeof(p->name), "%.*s", PROFILE_NAME_MAX, file);
		snprintf(p->error, sizeof(p->error), "invalid profile name");
		return;
	}
	if (read_file(dirfd, file, buf, sizeof(buf), &len, &st, err) < 0) {
		snprintf(p->name, sizeof(p->name), "%s", name);
		snprintf(p->error, sizeof(p->error), "%s", err);
		return;
	}
	profile_parse(p, name, buf, len, (st.st_mode & 077) != 0);
}

int conf_load(const char *dir, struct config *c) {
	memset(c, 0, sizeof(*c));
	int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", dir, strerror(errno));
		return -1;
	}
	DIR *d = fdopendir(dup(dirfd));
	if (!d) {
		fprintf(stderr, "cannot read %s: %s\n", dir, strerror(errno));
		close(dirfd);
		return -1;
	}

	load_ifaces(dirfd, &c->ifaces);

	char **names = NULL;
	size_t n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len <= 4 || strcmp(e->d_name + len - 4, ".tnc") != 0 ||
		    strcmp(e->d_name, TEMPLATE) == 0)
			continue;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 16;
			char **grown = realloc(names, ncap * sizeof(*names));
			if (!grown)
				break;
			names = grown;
			cap = ncap;
		}
		names[n] = strdup(e->d_name);
		if (names[n])
			n++;
	}
	closedir(d);

	if (n)
		qsort(names, n, sizeof(*names), by_name);
	for (size_t i = 0; i < n; i++) {
		if (i < PROFILES_MAX)
			load_profile(dirfd, names[i], &c->profiles[c->n++]);
		else
			c->truncated = 1;
		free(names[i]);
	}
	free(names);
	close(dirfd);
	return 0;
}
