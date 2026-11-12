#ifndef TUNDRA_SERVER_H
#define TUNDRA_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define SERVER_CLIENTS 16
/* The longest PROFILE-SET: a name, ten keys and a passphrase of 128 bytes
 * with every one as %XX stay under 800 bytes. */
#define SERVER_LINE_MAX 1024
#define SERVER_OUT_MAX 16384

struct client {
	int fd;
	int subscribed;
	int closing;
	uint32_t events;
	char in[SERVER_LINE_MAX];
	size_t inlen;
	char out[SERVER_OUT_MAX];
	size_t outlen;
};

struct server {
	int ep;
	int fd;
	char path[108];
	uint64_t tag_client;
	struct client c[SERVER_CLIENTS];
};

typedef void (*server_handler)(void *ctx, int client, const char *line);

int server_open(struct server *s, const char *path, int ep, uint64_t tag_listen,
                uint64_t tag_client);
void server_accept(struct server *s);
void server_input(struct server *s, int idx, server_handler h, void *ctx);
void server_output(struct server *s, int idx);
void server_send(struct server *s, int idx, const char *line);
void server_broadcast(struct server *s, const char *line);
void server_close(struct server *s);

#endif
