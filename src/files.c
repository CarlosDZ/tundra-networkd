#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int files_make_dirs(const char *dir, mode_t mode) {
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s", dir) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(path, mode) < 0 && errno != EEXIST ? -1 : 0;
}

int files_write_atomic(const char *path, const char *data, size_t len,
                       mode_t mode) {
	char tmp[PATH_MAX];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd =
	    open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
	if (fd < 0)
		return -1;
	size_t done = 0;
	while (done < len) {
		ssize_t w = write(fd, data + done, len - done);
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0)
			break;
		done += (size_t)w;
	}
	/* fchmod because the umask inherited from init may hide the file from
	 * the users that resolve names through it. */
	int failed = done < len || fchmod(fd, mode) < 0;
	int saved = errno;
	if (close(fd) < 0 && !failed) {
		failed = 1;
		saved = errno;
	}
	if (failed) {
		unlink(tmp);
		errno = saved;
		return -1;
	}
	if (rename(tmp, path) < 0) {
		int saved = errno;
		unlink(tmp);
		errno = saved;
		return -1;
	}
	return 0;
}
