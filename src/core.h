#ifndef TUNDRA_CORE_H
#define TUNDRA_CORE_H

#include "ifaces.h"
#include "load.h"
#include "profile.h"

#include <net/if.h>
#include <stdint.h>

#define CORE_IFACES 16
#define ACTIONS_MAX 32
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define STABLE_MS 60000
#define STOP_TIMEOUT_MS 5000

enum conn_state {
	C_IDLE,
	C_RUNNING,
	C_STOPPING,
	C_BACKOFF,
	C_WAIT_LINK,
	C_BLOCKED
};

enum block_reason { B_NONE, B_CONFIG, B_EXITED, B_ALERT, B_UNSUPPORTED };

struct core_iface {
	char name[IF_NAMESIZE];
	int present;
	int physical;
	enum conn_state st;
	char profile[PROFILE_NAME_MAX + 1];
	char pending[PROFILE_NAME_MAX + 1];
	int manual_off;
	enum block_reason blocked;
	unsigned int backoff_ms;
	uint64_t started_at;
	uint64_t retry_at;
	uint64_t stop_at;
};

struct core {
	const struct config *cfg;
	struct core_iface ifs[CORE_IFACES];
	int n;
	int shutting_down;
};

enum act_type { A_SPAWN_DHCPC, A_SIGNAL, A_LOG };

struct action {
	enum act_type type;
	char iface[IF_NAMESIZE];
	char profile[PROFILE_NAME_MAX + 1];
	int sig;
	int has_metric;
	unsigned int metric;
	int has_table;
	char table[TABLE_MAX + 1];
	unsigned int grace_ms;
	char msg[160];
};

struct actions {
	struct action a[ACTIONS_MAX];
	int n;
};

enum connect_result {
	CONNECT_OK,
	CONNECT_NO_PROFILE,
	CONNECT_INVALID,
	CONNECT_NO_MATCH,
	CONNECT_UNMANAGED,
	CONNECT_NO_IFACE,
	CONNECT_BLOCKED,
	CONNECT_ALERT_PENDING
};

void core_init(struct core *c, const struct config *cfg);
void core_link(struct core *c, const char *ifname, int physical, uint64_t now,
               struct actions *out);
void core_link_gone(struct core *c, const char *ifname, uint64_t now,
                    struct actions *out);
void core_child_exit(struct core *c, const char *ifname, int status,
                     uint64_t now, struct actions *out);
void core_timer(struct core *c, uint64_t now, struct actions *out);
enum connect_result core_connect(struct core *c, const char *profile,
                                 const char *ifname, uint64_t now,
                                 struct actions *out);
int core_disconnect(struct core *c, const char *ifname, uint64_t now,
                    struct actions *out);
void core_shutdown(struct core *c, uint64_t now, struct actions *out);
int core_done(const struct core *c);
uint64_t core_next_deadline(const struct core *c);
const char *core_unmanaged(const struct core *c, const char *ifname);
const struct core_iface *core_find(const struct core *c, const char *ifname);

#endif
