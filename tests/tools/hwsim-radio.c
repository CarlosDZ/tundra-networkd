#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* drivers/net/wireless/virtual/mac80211_hwsim.h, not exported to uapi */
#define HWSIM_FAMILY "MAC80211_HWSIM"
#define HWSIM_CMD_NEW_RADIO 4

struct req {
	struct nlmsghdr nh;
	struct genlmsghdr gh;
	unsigned char attrs[NLA_HDRLEN + NLA_ALIGN(sizeof(HWSIM_FAMILY))];
};

static char buf[16384];

static struct nlmsghdr *talk(int fd, const struct req *rq) {
	if (send(fd, rq, rq->nh.nlmsg_len, 0) < 0) {
		perror("send error");
		return NULL;
	}
	ssize_t n;
	do
		n = recv(fd, buf, sizeof(buf), 0);
	while (n < 0 && errno == EINTR);
	if (n < 0) {
		perror("recv error");
		return NULL;
	}
	struct nlmsghdr *nh = (struct nlmsghdr *)buf;
	if ((size_t)n < sizeof(*nh) || nh->nlmsg_len < sizeof(*nh) ||
	    nh->nlmsg_len > (size_t)n) {
		fprintf(stderr, "short netlink reply\n");
		return NULL;
	}
	if (nh->nlmsg_type == NLMSG_ERROR &&
	    nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
		fprintf(stderr, "short netlink error\n");
		return NULL;
	}
	return nh;
}

static int nl_error(const struct nlmsghdr *nh) {
	const struct nlmsgerr *e = NLMSG_DATA(nh);
	return e->error;
}

static int family_id(int fd) {
	struct req rq = {
	    .nh = {.nlmsg_type = GENL_ID_CTRL, .nlmsg_flags = NLM_F_REQUEST},
	    .gh = {.cmd = CTRL_CMD_GETFAMILY, .version = 1},
	};
	struct nlattr *a = (struct nlattr *)rq.attrs;
	a->nla_type = CTRL_ATTR_FAMILY_NAME;
	a->nla_len = NLA_HDRLEN + sizeof(HWSIM_FAMILY);
	memcpy(rq.attrs + NLA_HDRLEN, HWSIM_FAMILY, sizeof(HWSIM_FAMILY));
	rq.nh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_ALIGN(a->nla_len));

	struct nlmsghdr *nh = talk(fd, &rq);
	if (!nh)
		return -1;
	if (nh->nlmsg_type == NLMSG_ERROR) {
		fprintf(stderr, "no %s family: %s\n", HWSIM_FAMILY,
		        strerror(-nl_error(nh)));
		return -1;
	}
	size_t off = NLMSG_LENGTH(GENL_HDRLEN);
	while (off + NLA_HDRLEN <= nh->nlmsg_len) {
		const struct nlattr *at = (const struct nlattr *)((char *)nh + off);
		if (at->nla_len < NLA_HDRLEN || off + at->nla_len > nh->nlmsg_len)
			break;
		if ((at->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID &&
		    at->nla_len >= NLA_HDRLEN + sizeof(__u16)) {
			__u16 id;
			memcpy(&id, (const char *)at + NLA_HDRLEN, sizeof(id));
			return id;
		}
		off += NLA_ALIGN(at->nla_len);
	}
	fprintf(stderr, "no family id in the reply\n");
	return -1;
}

int main(int argc, char *argv[]) {
	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "usage: hwsim-radio\n");
		return 2;
	}
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0) {
		perror("socket error");
		return 1;
	}
	int id = family_id(fd);
	if (id < 0)
		return 1;

	struct req rq = {
	    .nh = {.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN),
	           .nlmsg_type = (__u16)id,
	           .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK},
	    .gh = {.cmd = HWSIM_CMD_NEW_RADIO, .version = 1},
	};
	struct nlmsghdr *nh = talk(fd, &rq);
	if (!nh)
		return 1;
	if (nh->nlmsg_type != NLMSG_ERROR) {
		fprintf(stderr, "unexpected netlink reply %u\n", nh->nlmsg_type);
		return 1;
	}
	/* The ack carries the new radio's index, or -errno */
	int err = nl_error(nh);
	if (err < 0) {
		fprintf(stderr, "cannot create a radio: %s\n", strerror(-err));
		return 1;
	}
	printf("%d\n", err);
	close(fd);
	return 0;
}
