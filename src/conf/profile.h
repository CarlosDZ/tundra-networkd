#ifndef TUNDRA_PROFILE_H
#define TUNDRA_PROFILE_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_NAME_MAX 64
#define CONF_ERR_MAX 128
#define SSID_MAX 32
#define PASSPHRASE_MAX 128
#define DNS_MAX 3
#define PROFILE_TEMPLATE "000-template"
#define PROFILE_TEXT_MAX 2048

enum security { SEC_NONE, SEC_OPEN, SEC_WPA2, SEC_WPA3 };
enum address_mode { ADDR_DHCP, ADDR_STATIC };
enum dns_mode { DNS_LEASE, DNS_LIST, DNS_NONE };
enum mac_mode { MAC_UNSET, MAC_KEEP, MAC_RANDOM, MAC_FIXED };

#define MAC_LEN 6

struct mac_policy {
	enum mac_mode mode;
	unsigned char addr[MAC_LEN];
};

struct profile {
	char name[PROFILE_NAME_MAX + 1];
	char interface[IF_NAMESIZE];
	int any_interface;
	unsigned char ssid[SSID_MAX];
	size_t ssid_len;
	int has_ssid;
	enum security security;
	char passphrase[PASSPHRASE_MAX + 1];
	int has_passphrase;
	enum address_mode address;
	uint32_t ip;
	int prefix;
	uint32_t gateway;
	int has_gateway;
	enum dns_mode dns;
	uint32_t dns_list[DNS_MAX];
	int ndns;
	int autoconnect;
	int trusted;
	struct mac_policy mac;
	int valid;
	char error[CONF_ERR_MAX];
};

enum profile_key {
	PKEY_INTERFACE,
	PKEY_SSID,
	PKEY_SECURITY,
	PKEY_PASSPHRASE,
	PKEY_ADDRESS,
	PKEY_GATEWAY,
	PKEY_DNS,
	PKEY_AUTOCONNECT,
	PKEY_TRUSTED,
	PKEY_MAC,
	PKEY_COUNT
};

/* A profile asked for key by key. named: the request carries the key. len 0
 * with named: the request says the key has no value. */
struct profile_draft {
	struct draft_key {
		int named;
		size_t len;
		char value[PASSPHRASE_MAX + 1];
	} key[PKEY_COUNT];
};

int conf_valid_ifname(const char *s);
int conf_parse_mac(const char *s, struct mac_policy *out);
int profile_name_from_file(const char *file, char name[PROFILE_NAME_MAX + 1]);
int profile_valid_name(const char *name);
int profile_parse(struct profile *p, const char *name, const char *text,
                  size_t len, int exposed);
int profile_key(const char *name);
void profile_draft_keep(struct profile_draft *d, const struct profile *old);
int profile_build(struct profile *p, const char *name,
                  const struct profile_draft *d, char *text, size_t size);

#endif
