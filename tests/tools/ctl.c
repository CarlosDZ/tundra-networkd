#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int send_all(int fd, const char *buf, size_t len) {
	while (len) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static int send_stdin(int fd) {
	char buf[4096];
	ssize_t n;
	while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
		if (send_all(fd, buf, (size_t)n) < 0)
			return -1;
	return n < 0 ? -1 : 0;
}

int main(int argc, char *argv[]) {
	int follow = 0, i = 1;
	if (argc > 1 && strcmp(argv[1], "-f") == 0) {
		follow = 1;
		i++;
	}
	if (argc - i < 1 || argc - i > 2) {
		fprintf(stderr, "usage: ctl [-f] <socket> [line]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[i]);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("connect error");
		return 2;
	}
	/* A server that turns the client away closes at once: the reply is still
	 * there to read after a failed send. */
	signal(SIGPIPE, SIG_IGN);
	const char *line = argc - i == 2 ? argv[i + 1] : NULL;
	if ((line
	         ? send_all(fd, line, strlen(line)) < 0 || send_all(fd, "\n", 1) < 0
	         : send_stdin(fd) < 0) &&
	    errno != EPIPE && errno != ECONNRESET) {
		perror("send error");
		return 2;
	}

	if (!line)
		shutdown(fd, SHUT_WR);

	char buf[8192];
	size_t len = 0;
	for (;;) {
		struct pollfd p = {.fd = fd, .events = POLLIN};
		int r = poll(&p, 1, follow ? -1 : 5000);
		if (r <= 0)
			return 3;
		ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
		if (n <= 0) {
			if (len)
				printf("%.*s\n", (int)len, buf);
			return 3;
		}
		len += (size_t)n;
		char *start = buf, *nl;
		while ((nl = memchr(start, '\n', len - (size_t)(start - buf)))) {
			*nl = '\0';
			printf("%s\n", start);
			if (line && !follow && strcmp(start, "OK") == 0)
				return 0;
			if (line && !follow && strncmp(start, "ERROR", 5) == 0)
				return 1;
			start = nl + 1;
		}
		len -= (size_t)(start - buf);
		memmove(buf, start, len);
	}
}
