#include "priv.h"

#include <linux/capability.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHILD_CAPS                                                             \
	((1ULL << CAP_NET_ADMIN) | (1ULL << CAP_NET_RAW) |                         \
	 (1ULL << CAP_NET_BIND_SERVICE))

struct priv_caps priv_shed_sets(struct priv_caps now) {
	struct priv_caps keep = {0};
	keep.inheritable = (now.permitted | now.inheritable) & CHILD_CAPS;
	return keep;
}

int priv_shed(void) {
	if (geteuid() == 0)
		return 0;

	struct __user_cap_header_struct hdr = {_LINUX_CAPABILITY_VERSION_3, 0};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	if (syscall(SYS_capget, &hdr, data) < 0) {
		perror("capget error");
		return -1;
	}
	struct priv_caps now = {
	    data[0].effective | ((uint64_t)data[1].effective << 32),
	    data[0].permitted | ((uint64_t)data[1].permitted << 32),
	    data[0].inheritable | ((uint64_t)data[1].inheritable << 32),
	};
	struct priv_caps keep = priv_shed_sets(now);
	for (int i = 0; i < _LINUX_CAPABILITY_U32S_3; i++) {
		data[i].effective = (uint32_t)(keep.effective >> (32 * i));
		data[i].permitted = (uint32_t)(keep.permitted >> (32 * i));
		data[i].inheritable = (uint32_t)(keep.inheritable >> (32 * i));
	}
	if (syscall(SYS_capset, &hdr, data) < 0) {
		perror("capset error");
		return -1;
	}
	/* An ambient capability would reach every child, marked or not. */
	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0) {
		perror("prctl(PR_CAP_AMBIENT) error");
		return -1;
	}
	return 0;
}
