#ifndef TUNDRA_CORE_H
#define TUNDRA_CORE_H

#include "alert.h"
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
#define SENSOR_FAILS_MAX 3
#define SENSOR_DOWN_LEVEL 5
#define ALERT_ALL_LEVEL 9
#define TRUST_LEVEL 6
#define STATIC_TAKEN_LEVEL 2
#define STATIC_FAIL (-1)
#define STATIC_USAGE (-2)
#define ACCEPTS_MAX 8
#define WIFI_GRACE_MS 3000
#define BSSID_MAX 18
#define LINK_PHYSICAL 1
#define LINK_WIRELESS 2
#define LINK_UP 4

enum conn_state {
	C_IDLE,
	C_RUNNING,
	C_STOPPING,
	C_BACKOFF,
	C_WAIT_LINK,
	C_BLOCKED,
	C_PROBING,
	C_STATIC
};

enum sensor_state { S_OFF, S_RUNNING, S_STOPPING, S_BACKOFF };
enum wpa_state { W_OFF, W_RUNNING, W_STOPPING, W_BACKOFF };
enum child_kind { K_DHCPC, K_SENSOR, K_PROBE, K_WPA };
enum probe_result { PROBE_NONE, PROBE_FREE, PROBE_TAKEN };
enum block_reason { B_NONE, B_CONFIG, B_EXITED, B_ALERT, B_TAKEN };

struct accept {
	char kase[ALERT_TOKEN_MAX + 1];
	int level;
};

struct core_iface {
	char name[IF_NAMESIZE];
	int present;
	int physical;
	int wireless;
	int up;
	enum conn_state st;
	char profile[PROFILE_NAME_MAX + 1];
	char pending[PROFILE_NAME_MAX + 1];
	int manual_off;
	enum block_reason blocked;
	enum child_kind stop_kind;
	unsigned int backoff_ms;
	uint64_t started_at;
	uint64_t retry_at;
	uint64_t stop_at;
	struct profile conn;
	struct iface_conf conn_conf;
	int has_conn_conf;
	char kept[CONF_ERR_MAX + 48];
	int alert_stop;
	enum sensor_state sst;
	uint32_t want_addr;
	uint32_t want_gw;
	uint32_t conflict_addr;
	uint32_t s_addr;
	uint32_t s_gw;
	unsigned int s_backoff_ms;
	int s_fails;
	uint64_t s_started_at;
	uint64_t s_retry_at;
	uint64_t s_stop_at;
	struct accept accepts[ACCEPTS_MAX];
	int naccepts;
	int dispute;
	unsigned int override;
	unsigned int stop_id;
	enum wpa_state wst;
	char wselect[PROFILE_NAME_MAX + 1];
	unsigned char cssid[SSID_MAX];
	size_t cssid_len;
	int associated;
	unsigned char assid[SSID_MAX];
	size_t assid_len;
	char bssid[BSSID_MAX];
	unsigned int w_backoff_ms;
	uint64_t w_started_at;
	uint64_t w_retry_at;
	uint64_t w_stop_at;
};

struct core {
	const struct config *cfg;
	struct core_iface ifs[CORE_IFACES];
	int n;
	int shutting_down;
	int lockdown;
	unsigned int lockdown_id;
};

enum act_type {
	A_SPAWN_DHCPC,
	A_SPAWN_SENSOR,
	A_SPAWN_PROBE,
	A_SPAWN_WPA,
	A_WPA_SYNC,
	A_LINK_DOWN,
	A_STATIC_APPLY,
	A_STATIC_REMOVE,
	A_SIGNAL,
	A_LOG,
	A_ALERT
};

struct action {
	enum act_type type;
	enum child_kind kind;
	char iface[IF_NAMESIZE];
	char profile[PROFILE_NAME_MAX + 1];
	int sig;
	int has_metric;
	unsigned int metric;
	int has_table;
	char table[TABLE_MAX + 1];
	unsigned int grace_ms;
	uint32_t addr;
	int prefix;
	uint32_t gw;
	int probe_off;
	int defend_always;
	char accept[ACCEPTS_MAX * (ALERT_TOKEN_MAX + 4)];
	enum alert_action alert;
	int level;
	char kase[ALERT_TOKEN_MAX + 1];
	char msg[160];
};

struct actions {
	struct action a[ACTIONS_MAX];
	int n;
};

struct wifi_net {
	const struct profile *p;
	int priority;
	int enabled;
};

enum connect_result {
	CONNECT_OK,
	CONNECT_NO_PROFILE,
	CONNECT_INVALID,
	CONNECT_NO_MATCH,
	CONNECT_UNMANAGED,
	CONNECT_NO_IFACE,
	CONNECT_BLOCKED,
	CONNECT_ALERT_PENDING,
	CONNECT_OFF
};

void core_init(struct core *c, const struct config *cfg);
void core_link(struct core *c, const char *ifname, unsigned int flags,
               uint64_t now, struct actions *out);
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
void core_lease(struct core *c, const char *ifname, uint32_t addr, uint32_t gw,
                uint64_t now, struct actions *out);
void core_sensor_exit(struct core *c, const char *ifname, int status,
                      const char *conflict_mac, uint64_t now,
                      struct actions *out);
void core_probe_exit(struct core *c, const char *ifname, int status,
                     enum probe_result result, const char *mac, const char *src,
                     uint64_t now, struct actions *out);
void core_static_result(struct core *c, const char *ifname, int rc,
                        uint64_t now, struct actions *out);
void core_wpa_exit(struct core *c, const char *ifname, int status, uint64_t now,
                   struct actions *out);
void core_wifi_assoc(struct core *c, const char *ifname, const char *id_str,
                     const unsigned char *ssid, size_t ssid_len,
                     const char *bssid, uint64_t now, struct actions *out);
void core_wifi_lost(struct core *c, const char *ifname);
void core_stop(struct core *c, const char *ifname, int level, unsigned int id,
               uint64_t now, struct actions *out);
void core_note_alert(struct core *c, const char *ifname, int level,
                     unsigned int id);
int core_force(struct core *c, unsigned int id, const char *origin,
               const char *kase, int level, uint64_t now, struct actions *out);
int core_release_alert(struct core *c, unsigned int id, struct actions *out);
void core_restore_stop(struct core *c, const char *ifname, const char *profile,
                       int level, unsigned int id);
void core_reload(struct core *c, const struct config *cfg, uint64_t now,
                 struct actions *out);
int core_done(const struct core *c);
uint64_t core_next_deadline(const struct core *c);
const char *core_unmanaged(const struct core *c, const char *ifname);
int core_wifi_off(const struct core *c, const char *ifname);
int core_wifi_nets(const struct core *c, const char *ifname,
                   struct wifi_net *out, int max, int *disconnected);
const struct core_iface *core_find(const struct core *c, const char *ifname);

#endif
