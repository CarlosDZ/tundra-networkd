#include "files.c"
#include "leaseinfo.c"
#include "resolv.c"
#include "test.h"

#include <arpa/inet.h>
#include <sys/stat.h>

static uint32_t ip(const char *s) {
	struct in_addr a;
	inet_pton(AF_INET, s, &a);
	return a.s_addr;
}

static struct resolv_src src(const char *ifname, unsigned int metric,
                             const char *a, const char *b, const char *c) {
	struct resolv_src r = {.metric = metric};
	snprintf(r.ifname, sizeof(r.ifname), "%s", ifname);
	const char *list[] = {a, b, c};
	for (int i = 0; i < DNS_MAX && list[i]; i++)
		r.ns[r.n++] = ip(list[i]);
	return r;
}

static int built(struct resolv_src *s, int n, const char *expect) {
	char out[RESOLV_TEXT_MAX];
	return resolv_build(s, n, out, sizeof(out)) >= 0 &&
	       strcmp(out, expect) == 0;
}

static struct lease_info li;

static int lease_dns(const char *text, int expect_n, const char *first) {
	leaseinfo_parse(text, strlen(text), &li);
	return li.ndns == expect_n && (!first || li.dns[0] == ip(first));
}

static int lease_addr(const char *text, const char *expect) {
	char a[LEASEINFO_ADDR_MAX];
	if (leaseinfo_parse(text, strlen(text), &li) < 0)
		return expect == NULL;
	leaseinfo_address(&li, a);
	return expect && strcmp(a, expect) == 0;
}

static char dir[] = "/tmp/tnd-resolv.XXXXXX";

static int file_is(const char *path, const char *expect, mode_t mode) {
	char buf[256] = "";
	struct stat st;
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return stat(path, &st) == 0 && (st.st_mode & 0777) == mode &&
	       strcmp(buf, expect) == 0;
}

int main(void) {
	CHECK(lease_dns("address=10.0.0.5\nmask=255.255.255.0\n"
	                "dns=10.0.0.1,10.0.0.2\nlease=3600\n",
	                2, "10.0.0.1"),
	      "the dns line of a lease");
	CHECK(lease_dns("address=10.0.0.5\nlease=3600\n", 0, NULL),
	      "a lease without dns");
	CHECK(lease_dns("dns=10.0.0.1,10.0.0.2,10.0.0.3", 3, "10.0.0.1"),
	      "a dns line at the end of the file without newline");
	CHECK(lease_dns("dns=10.0.0.1,nope\n", 0, NULL),
	      "a malformed dns line gives no servers");
	CHECK(lease_dns("dnsx=10.0.0.1\n", 0, NULL), "only the dns key counts");
	CHECK(lease_addr("address=10.79.0.100\nmask=255.255.255.0\n",
	                 "10.79.0.100/24"),
	      "address and mask give the prefix");
	CHECK(lease_addr("address=10.0.0.1\nmask=255.255.255.255\n", "10.0.0.1/32"),
	      "a /32 lease");
	CHECK(lease_addr("mask=255.255.255.0\ndns=10.0.0.1\n", NULL),
	      "a lease without address is rejected");
	CHECK(lease_addr("address=10.0.0.1\nmask=255.0.255.0\n", NULL),
	      "a non contiguous mask is rejected");
	CHECK(lease_addr("address=10.0.0.1\n", NULL),
	      "a lease without mask is rejected");

	struct profile p = {.dns = DNS_LEASE};
	struct iface_conf conf = {.has_metric = 1, .metric = 300};
	struct resolv_src r;
	struct lease_info lease = {.ndns = 1, .dns = {ip("10.0.0.1")}};
	CHECK(resolv_source("tnd0", &p, &conf, NULL, &r) < 0,
	      "no lease, no source");
	CHECK(resolv_source("tnd0", NULL, &conf, &lease, &r) < 0,
	      "an unknown profile is no source");
	CHECK(resolv_source("tnd0", &p, &conf, &lease, &r) == 0 &&
	          r.metric == 300 && r.n == 1 && r.ns[0] == ip("10.0.0.1") &&
	          strcmp(r.ifname, "tnd0") == 0,
	      "dns = lease takes the servers of the lease");
	CHECK(resolv_source("tnd0", &p, NULL, &lease, &r) == 0 && r.metric == 0,
	      "an interface without metric counts as 0");
	p.dns = DNS_LIST;
	p.ndns = 2;
	p.dns_list[0] = ip("9.9.9.9");
	p.dns_list[1] = ip("1.1.1.1");
	CHECK(resolv_source("tnd0", &p, &conf, &lease, &r) == 0 && r.n == 2 &&
	          r.ns[0] == ip("9.9.9.9"),
	      "a dns list replaces the lease");
	p.dns = DNS_NONE;
	CHECK(resolv_source("tnd0", &p, &conf, &lease, &r) == 0 && r.n == 0,
	      "dns = none is a source without servers");

	struct resolv_src s[4];
	CHECK(built(s, 0, ""), "no connections, empty file");
	s[0] = src("tnd0", 600, "10.0.0.1", "10.0.0.2", NULL);
	s[1] = src("tnd1", 100, "10.0.0.3", "10.0.0.4", NULL);
	CHECK(built(s, 2,
	            "nameserver 10.0.0.3\nnameserver 10.0.0.4\n"
	            "nameserver 10.0.0.1\n"),
	      "the lowest metric goes first, three servers at most");
	s[0] = src("tnd1", 100, "10.0.0.2", NULL, NULL);
	s[1] = src("tnd0", 100, "10.0.0.1", NULL, NULL);
	CHECK(built(s, 2, "nameserver 10.0.0.1\nnameserver 10.0.0.2\n"),
	      "a tie in metric goes by interface name");
	s[0] = src("tnd0", 100, "10.0.0.1", "10.0.0.2", NULL);
	s[1] = src("tnd1", 600, "10.0.0.2", "10.0.0.1", "10.0.0.9");
	CHECK(built(s, 2,
	            "nameserver 10.0.0.1\nnameserver 10.0.0.2\n"
	            "nameserver 10.0.0.9\n"),
	      "a repeated server is listed once");
	s[0] = src("tnd0", 100, NULL, NULL, NULL);
	s[1] = src("tnd1", 600, "10.0.0.7", NULL, NULL);
	CHECK(built(s, 2, "nameserver 10.0.0.7\n"),
	      "a connection without servers gives way to the next");
	s[0] = src("tnd0", 100, "255.255.255.255", "255.255.255.254",
	           "255.255.255.253");
	CHECK(built(s, 1,
	            "nameserver 255.255.255.255\nnameserver 255.255.255.254\n"
	            "nameserver 255.255.255.253\n"),
	      "the longest addresses fit");

	if (!mkdtemp(dir))
		return 1;
	char path[256], sub[256], tmp[300];
	snprintf(path, sizeof(path), "%s/resolv.conf", dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	umask(077);
	CHECK(files_write_atomic(path, "nameserver 10.0.0.1\n", 20, 0644) == 0 &&
	          file_is(path, "nameserver 10.0.0.1\n", 0644),
	      "atomic write with its mode despite the umask");
	CHECK(files_write_atomic(path, "", 0, 0644) == 0 && file_is(path, "", 0644),
	      "an empty write replaces the file");
	CHECK(access(tmp, F_OK) < 0, "no temporary file left behind");
	snprintf(sub, sizeof(sub), "%s/a/b", dir);
	CHECK(files_make_dirs(sub, 0700) == 0 && files_make_dirs(sub, 0700) == 0,
	      "nested directories, twice");
	snprintf(sub, sizeof(sub), "%s/a/b/c/x", dir);
	CHECK(files_write_atomic(sub, "x", 1, 0644) < 0,
	      "a write into a missing directory fails");
	snprintf(sub, sizeof(sub), "%s/a/b", dir);
	rmdir(sub);
	snprintf(sub, sizeof(sub), "%s/a", dir);
	rmdir(sub);
	unlink(path);
	rmdir(dir);
	return test_summary();
}
