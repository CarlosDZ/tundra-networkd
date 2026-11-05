#include "link.h"

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int link_open(void) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
	                NETLINK_ROUTE);
	if (fd < 0) {
		perror("netlink socket error");
		return -1;
	}
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("netlink bind error");
		close(fd);
		return -1;
	}
	return fd;
}

int link_dump(int fd) {
	struct {
		struct nlmsghdr nlh;
		struct ifinfomsg ifi;
	} req;
	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETLINK;
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.ifi.ifi_family = AF_UNSPEC;
	if (send(fd, &req, sizeof(req), 0) < 0) {
		perror("netlink dump error");
		return -1;
	}
	return 0;
}

static int parse_link(struct nlmsghdr *nlh, struct link_ev *ev) {
	struct ifinfomsg *ifi = NLMSG_DATA(nlh);
	int len = (int)nlh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifi));
	if (len < 0)
		return -1;

	memset(ev, 0, sizeof(*ev));
	ev->gone = nlh->nlmsg_type == RTM_DELLINK;
	ev->ifindex = ifi->ifi_index;
	ev->arptype = ifi->ifi_type;
	ev->up = (ifi->ifi_flags & IFF_UP) != 0;
	for (struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, len);
	     rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type != IFLA_IFNAME)
			continue;
		size_t n = RTA_PAYLOAD(rta);
		if (n == 0 || n > IF_NAMESIZE)
			return -1;
		memcpy(ev->name, RTA_DATA(rta), n);
		ev->name[n - 1] = '\0';
	}
	return ev->name[0] ? 0 : -1;
}

int link_read(int fd, struct link_ev *evs, int max) {
	char buf[16384];
	ssize_t len = recv(fd, buf, sizeof(buf), 0);
	if (len < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;

	int n = 0;
	for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	     NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
		if (nlh->nlmsg_type != RTM_NEWLINK && nlh->nlmsg_type != RTM_DELLINK)
			continue;
		if (n < max && parse_link(nlh, &evs[n]) == 0)
			n++;
	}
	return n;
}
