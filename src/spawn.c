#include "spawn.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

static void reset_signals(void) {
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (int sig = 1; sig < NSIG; sig++)
		if (sig != SIGKILL && sig != SIGSTOP)
			signal(sig, SIG_DFL);
}

pid_t spawn_child(const char *path, char *const argv[], int *out) {
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		perror("pipe error");
		return -1;
	}
	pid_t parent = getpid();
	fflush(stdout);

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork error");
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		reset_signals();
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent)
			_exit(1);
		dup2(p[1], STDOUT_FILENO);
		dup2(p[1], STDERR_FILENO);
		execv(path, argv);
		perror("execv error");
		_exit(127);
	}

	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	*out = p[0];
	return pid;
}
