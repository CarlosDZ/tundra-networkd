#include "spawn.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <termios.h>
#include <unistd.h>

static void reset_signals(void) {
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (int sig = 1; sig < NSIG; sig++)
		if (sig != SIGKILL && sig != SIGSTOP)
			signal(sig, SIG_DFL);
}

static pid_t start(const char *path, char *const argv[], int rd, int wr,
                   int *out) {
	pid_t parent = getpid();
	fflush(stdout);

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork error");
		close(rd);
		close(wr);
		return -1;
	}
	if (pid == 0) {
		reset_signals();
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent)
			_exit(1);
		dup2(wr, STDOUT_FILENO);
		dup2(wr, STDERR_FILENO);
		execv(path, argv);
		perror("execv error");
		_exit(127);
	}

	close(wr);
	fcntl(rd, F_SETFL, O_NONBLOCK);
	*out = rd;
	return pid;
}

pid_t spawn_child(const char *path, char *const argv[], int *out) {
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		perror("pipe error");
		return -1;
	}
	return start(path, argv, p[0], p[1], out);
}

pid_t spawn_child_tty(const char *path, char *const argv[], int *out) {
	int m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	int s = m < 0 || unlockpt(m) < 0
	            ? -1
	            : ioctl(m, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (s < 0) {
		perror("pty error");
		if (m >= 0)
			close(m);
		return -1;
	}
	struct termios t;
	if (tcgetattr(s, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(s, TCSANOW, &t);
	}
	return start(path, argv, m, s, out);
}
