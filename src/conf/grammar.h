#ifndef TUNDRA_GRAMMAR_H
#define TUNDRA_GRAMMAR_H

#include <stddef.h>

#define GR_NAME_MAX 32
#define GR_VALUE_MAX 256
#define GR_ITEMS_MAX 64
#define GR_BLOCKS_MAX 32
#define GR_LINE_MAX 512

struct gr_item {
	int block;
	char key[GR_NAME_MAX];
	char value[GR_VALUE_MAX];
	size_t len;
	int quoted;
	int line;
};

struct gr_block {
	char name[GR_NAME_MAX];
	int line;
};

struct grammar {
	struct gr_item items[GR_ITEMS_MAX];
	int n;
	struct gr_block blocks[GR_BLOCKS_MAX];
	int nblocks;
	int err_line;
	char err[96];
};

int gr_parse(const char *text, size_t len, struct grammar *g);

#endif
