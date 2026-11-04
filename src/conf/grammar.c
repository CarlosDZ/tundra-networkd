#include "grammar.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int gr_fail(struct grammar *g, int line, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g->err, sizeof(g->err), fmt, ap);
	va_end(ap);
	g->err_line = line;
	return -1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int is_name(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static const char *skip_space(const char *p, const char *end) {
	while (p < end && is_space(*p))
		p++;
	return p;
}

static int rest_is_empty(const char *p, const char *end) {
	p = skip_space(p, end);
	return p == end || *p == '#';
}

static size_t read_name(const char *p, const char *end, char out[GR_NAME_MAX]) {
	size_t n = 0;
	while (p + n < end && is_name(p[n])) {
		if (n + 1 >= GR_NAME_MAX)
			return 0;
		out[n] = p[n];
		n++;
	}
	out[n] = '\0';
	return n;
}

static int hex(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int read_quoted(struct grammar *g, int line, const char *p,
                       const char *end, struct gr_item *it) {
	size_t n = 0;
	p++;
	while (p < end && *p != '"') {
		char c = *p++;
		if (c == '\\') {
			if (p == end)
				return gr_fail(g, line, "unterminated escape");
			char e = *p++;
			if (e == '"' || e == '\\') {
				c = e;
			} else if (e == 'x') {
				int h = p < end ? hex(*p) : -1;
				int l = p + 1 < end ? hex(p[1]) : -1;
				if (h < 0 || l < 0)
					return gr_fail(g, line, "invalid \\x escape");
				p += 2;
				c = (char)(h * 16 + l);
				if (c == '\0')
					return gr_fail(g, line, "\\x00 is not allowed");
			} else {
				return gr_fail(g, line, "unknown escape \\%c", e);
			}
		}
		if (n + 1 >= GR_VALUE_MAX)
			return gr_fail(g, line, "value too long");
		it->value[n++] = c;
	}
	if (p == end)
		return gr_fail(g, line, "unterminated quote");
	if (!rest_is_empty(p + 1, end))
		return gr_fail(g, line, "unexpected text after the closing quote");
	it->value[n] = '\0';
	it->len = n;
	it->quoted = 1;
	return 0;
}

static int read_bare(struct grammar *g, int line, const char *p,
                     const char *end, struct gr_item *it) {
	const char *stop = p;
	while (stop < end && *stop != '#')
		stop++;
	while (stop > p && is_space(stop[-1]))
		stop--;
	size_t n = (size_t)(stop - p);
	if (n == 0)
		return gr_fail(g, line, "missing value");
	if (memchr(p, '"', n))
		return gr_fail(g, line, "unexpected quote");
	if (n >= GR_VALUE_MAX)
		return gr_fail(g, line, "value too long");
	memcpy(it->value, p, n);
	it->value[n] = '\0';
	it->len = n;
	it->quoted = 0;
	return 0;
}

static int parse_line(struct grammar *g, int line, const char *p,
                      const char *end, int *open) {
	p = skip_space(p, end);
	if (p == end || *p == '#')
		return 0;

	if (*p == '}') {
		if (*open < 0)
			return gr_fail(g, line, "'}' without an open block");
		if (!rest_is_empty(p + 1, end))
			return gr_fail(g, line, "unexpected text after '}'");
		*open = -1;
		return 0;
	}

	char name[GR_NAME_MAX];
	size_t n = read_name(p, end, name);
	if (n == 0)
		return gr_fail(g, line, "expected a name");
	p = skip_space(p + n, end);

	if (p < end && *p == '{') {
		if (*open >= 0)
			return gr_fail(g, line, "nested block");
		if (!rest_is_empty(p + 1, end))
			return gr_fail(g, line, "unexpected text after '{'");
		if (g->nblocks == GR_BLOCKS_MAX)
			return gr_fail(g, line, "too many blocks");
		struct gr_block *b = &g->blocks[g->nblocks];
		memcpy(b->name, name, sizeof(b->name));
		b->line = line;
		*open = g->nblocks++;
		return 0;
	}

	if (p == end || *p != '=')
		return gr_fail(g, line, "expected '=' after %s", name);
	if (g->n == GR_ITEMS_MAX)
		return gr_fail(g, line, "too many lines");
	struct gr_item *it = &g->items[g->n];
	memset(it, 0, sizeof(*it));
	memcpy(it->key, name, sizeof(it->key));
	it->block = *open;
	it->line = line;
	p = skip_space(p + 1, end);
	int r = (p < end && *p == '"') ? read_quoted(g, line, p, end, it)
	                               : read_bare(g, line, p, end, it);
	if (r < 0)
		return r;
	g->n++;
	return 0;
}

int gr_parse(const char *text, size_t len, struct grammar *g) {
	memset(g, 0, sizeof(*g));
	const char *p = text, *end = text + len;
	int line = 0, open = -1;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *eol = nl ? nl : end;
		line++;
		if (eol - p > GR_LINE_MAX)
			return gr_fail(g, line, "line too long");
		if (memchr(p, '\0', (size_t)(eol - p)))
			return gr_fail(g, line, "NUL byte");
		if (parse_line(g, line, p, eol, &open) < 0)
			return -1;
		p = nl ? nl + 1 : end;
	}
	if (open >= 0)
		return gr_fail(g, g->blocks[open].line, "block %s is not closed",
		               g->blocks[open].name);
	return 0;
}
