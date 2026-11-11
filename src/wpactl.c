#include "wpactl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int wpactl_decode(const char *in, size_t len, unsigned char *out, size_t max,
                  size_t *outlen) {
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '\\') {
			if (++i == len)
				return -1;
			switch (in[i]) {
			case '\\':
			case '"':
				c = (unsigned char)in[i];
				break;
			case 'e':
				c = 033;
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'x': {
				int hi, lo;
				if (i + 2 >= len || (hi = hexval(in[i + 1])) < 0 ||
				    (lo = hexval(in[i + 2])) < 0)
					return -1;
				c = (unsigned char)(hi << 4 | lo);
				i += 2;
				break;
			}
			default:
				return -1;
			}
		}
		if (n == max)
			return -1;
		out[n++] = c;
	}
	*outlen = n;
	return 0;
}

static int has(const char *s, size_t len, const char *word) {
	size_t n = strlen(word);
	for (size_t i = 0; i + n <= len; i++)
		if (memcmp(s + i, word, n) == 0)
			return 1;
	return 0;
}

/* The flags of SCAN_RESULTS: [WPA2-PSK+SAE-CCMP][ESS], [WPA-PSK-TKIP], [WEP].
 */
static enum wpa_sec sec_of(const char *f, size_t len) {
	if (has(f, len, "EAP"))
		return WSEC_ENTERPRISE;
	if (has(f, len, "SAE"))
		return has(f, len, "PSK") ? WSEC_WPA2_WPA3 : WSEC_WPA3;
	if (has(f, len, "[WPA2-") || has(f, len, "[RSN-"))
		return WSEC_WPA2;
	if (has(f, len, "[WPA-"))
		return WSEC_WPA;
	if (has(f, len, "OWE"))
		return WSEC_OWE;
	if (has(f, len, "WEP"))
		return WSEC_WEP;
	return WSEC_OPEN;
}

static int is_bssid(const char *s, size_t len) {
	if (len != 17)
		return 0;
	for (size_t i = 0; i < len; i++)
		if (i % 3 == 2 ? s[i] != ':' : hexval(s[i]) < 0)
			return 0;
	return 1;
}

static int number(const char *s, size_t len, int *out) {
	char buf[12], *end;
	if (len == 0 || len >= sizeof(buf))
		return -1;
	memcpy(buf, s, len);
	buf[len] = '\0';
	long v = strtol(buf, &end, 10);
	if (*end || v < -1000000 || v > 1000000)
		return -1;
	*out = (int)v;
	return 0;
}

/* One access point per line: bssid, frequency, signal, flags and ssid, tab
 * separated. Lines that do not parse, the header among them, are skipped. */
int wpactl_scan_results(const char *text, size_t len, struct wpa_bss *out,
                        int max) {
	const char *p = text, *end = text + len;
	int n = 0;
	while (p < end && n < max) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol)
			eol = end;
		const char *f[5];
		size_t fl[5];
		int nf = 0;
		for (const char *q = p; nf < 5; nf++) {
			const char *tab =
			    nf < 4 ? memchr(q, '\t', (size_t)(eol - q)) : NULL;
			f[nf] = q;
			fl[nf] = (size_t)((tab ? tab : eol) - q);
			if (!tab) {
				nf++;
				break;
			}
			q = tab + 1;
		}
		p = eol < end ? eol + 1 : end;
		struct wpa_bss *b = &out[n];
		memset(b, 0, sizeof(*b));
		if (nf < 4 || !is_bssid(f[0], fl[0]) ||
		    number(f[1], fl[1], &b->freq) < 0 ||
		    number(f[2], fl[2], &b->signal) < 0)
			continue;
		if (nf == 5 && wpactl_decode(f[4], fl[4], b->ssid, sizeof(b->ssid),
		                             &b->ssid_len) < 0)
			continue;
		int hidden = 1;
		for (size_t i = 0; i < b->ssid_len; i++)
			hidden &= b->ssid[i] == 0;
		if (hidden)
			b->ssid_len = 0;
		memcpy(b->bssid, f[0], fl[0]);
		b->sec = sec_of(f[3], fl[3]);
		n++;
	}
	return n;
}

void wpactl_hex(const unsigned char *in, size_t len, char *out) {
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * len] = '\0';
}

static void copy_field(char *out, size_t size, const char *s, size_t len) {
	if (len >= size)
		len = size - 1;
	memcpy(out, s, len);
	out[len] = '\0';
}

static const char *find_key(const char *s, const char *end, const char *key,
                            size_t *vlen) {
	size_t klen = strlen(key);
	for (const char *p = s; p + klen <= end; p++) {
		if ((p == s || p[-1] == ' ' || p[-1] == '[') &&
		    strncmp(p, key, klen) == 0) {
			const char *v = p + klen;
			const char *stop = v;
			while (stop < end && *stop != ' ' && *stop != ']')
				stop++;
			*vlen = (size_t)(stop - v);
			return v;
		}
	}
	return NULL;
}

int wpactl_event(const char *msg, size_t len, struct wpa_event *ev) {
	const char *end = msg + len, *p = msg, *v;
	size_t vlen;
	memset(ev, 0, sizeof(*ev));
	ev->id = -1;
	if (p == end || *p != '<')
		return -1;
	while (p < end && *p != '>')
		p++;
	if (p == end)
		return -1;
	p++;

	static const struct {
		const char *name;
		enum wpa_event_type type;
	} names[] = {
	    {"CTRL-EVENT-CONNECTED ", WPA_EV_CONNECTED},
	    {"CTRL-EVENT-DISCONNECTED ", WPA_EV_DISCONNECTED},
	    {"CTRL-EVENT-SSID-TEMP-DISABLED ", WPA_EV_TEMP_DISABLED},
	    {"CTRL-EVENT-TERMINATING", WPA_EV_TERMINATING},
	    {"CTRL-EVENT-SCAN-RESULTS", WPA_EV_SCAN_RESULTS},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		size_t n = strlen(names[i].name);
		if ((size_t)(end - p) >= n && strncmp(p, names[i].name, n) == 0) {
			ev->type = names[i].type;
			break;
		}
	}

	if (ev->type == WPA_EV_CONNECTED) {
		const char *to = find_key(p, end, "to ", &vlen);
		if (to && vlen < WPA_BSSID_MAX)
			copy_field(ev->bssid, sizeof(ev->bssid), to, vlen);
	} else if ((v = find_key(p, end, "bssid=", &vlen)) &&
	           vlen < WPA_BSSID_MAX) {
		copy_field(ev->bssid, sizeof(ev->bssid), v, vlen);
	}
	if ((v = find_key(p, end, "id=", &vlen)))
		ev->id = atoi(v);
	if ((v = find_key(p, end, "id_str=", &vlen)))
		copy_field(ev->id_str, sizeof(ev->id_str), v, vlen);
	if ((v = find_key(p, end, "reason=", &vlen)))
		ev->reason = atoi(v);
	return 0;
}

int wpactl_status(const char *text, size_t len, struct wpa_status *st) {
	const char *end = text + len;
	memset(st, 0, sizeof(*st));
	st->id = -1;
	for (const char *line = text; line < end;) {
		const char *nl = memchr(line, '\n', (size_t)(end - line));
		const char *stop = nl ? nl : end;
		const char *eq = memchr(line, '=', (size_t)(stop - line));
		if (eq) {
			size_t klen = (size_t)(eq - line), vlen = (size_t)(stop - eq - 1);
			const char *v = eq + 1;
			if (klen == 9 && strncmp(line, "wpa_state", 9) == 0)
				copy_field(st->state, sizeof(st->state), v, vlen);
			else if (klen == 5 && strncmp(line, "bssid", 5) == 0)
				copy_field(st->bssid, sizeof(st->bssid), v, vlen);
			else if (klen == 4 && strncmp(line, "ssid", 4) == 0 &&
			         wpactl_decode(v, vlen, st->ssid, sizeof(st->ssid),
			                       &st->ssid_len) < 0)
				return -1;
			else if (klen == 2 && strncmp(line, "id", 2) == 0)
				st->id = atoi(v);
			else if (klen == 6 && strncmp(line, "id_str", 6) == 0)
				copy_field(st->id_str, sizeof(st->id_str), v, vlen);
		}
		line = stop + 1;
	}
	return st->state[0] ? 0 : -1;
}

static void add_var(struct wpa_network *w, int optional, const char *fmt,
                    const char *value) {
	if (w->n == WPA_VARS_MAX)
		return;
	snprintf(w->vars[w->n], WPA_VAR_MAX, fmt, value);
	w->optional[w->n++] = optional;
}

/* The password goes in hex, like the SSID, so no byte needs quoting. The
 * WPA2 passphrase stays quoted: psk takes hex only as the derived key. */
void wpactl_network(const struct profile *p, struct wpa_network *w) {
	char hex[2 * PASSPHRASE_MAX + 1];
	memset(w, 0, sizeof(*w));
	wpactl_hex(p->ssid, p->ssid_len, hex);
	add_var(w, 0, "ssid %s", hex);
	add_var(w, 0, "id_str \"%s\"", p->name);
	switch (p->security) {
	case SEC_WPA3:
		wpactl_hex((const unsigned char *)p->passphrase, strlen(p->passphrase),
		           hex);
		add_var(w, 0, "key_mgmt %s", "SAE");
		add_var(w, 0, "proto %s", "RSN");
		add_var(w, 0, "pairwise %s", "CCMP");
		add_var(w, 0, "ieee80211w %s", "2");
		add_var(w, 0, "sae_password %s", hex);
		add_var(w, 1, "ocv %s", "1");
		break;
	case SEC_WPA2:
		add_var(w, 0, "key_mgmt %s", "WPA-PSK");
		add_var(w, 0, "proto %s", "RSN");
		add_var(w, 0, "pairwise %s", "CCMP");
		add_var(w, 0, "ieee80211w %s", "1");
		add_var(w, 0, "psk \"%s\"", p->passphrase);
		break;
	case SEC_OPEN:
	case SEC_NONE:
		add_var(w, 0, "key_mgmt %s", "NONE");
		break;
	}
	uint64_t h = 0xcbf29ce484222325ULL;
	for (int i = 0; i < w->n; i++)
		for (const char *c = w->vars[i];; c++) {
			h = (h ^ (unsigned char)*c) * 0x100000001b3ULL;
			if (!*c)
				break;
		}
	w->hash = h;
}
