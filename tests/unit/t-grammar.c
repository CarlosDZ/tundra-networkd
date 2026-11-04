#include "conf/grammar.c"
#include "test.h"

static struct grammar g;

static int parse(const char *s) { return gr_parse(s, strlen(s), &g); }

static void test_basic(void) {
	CHECK(parse("") == 0 && g.n == 0, "an empty file is valid");
	CHECK(parse("# comment\n\n   # indented\n") == 0 && g.n == 0,
	      "comments and blank lines are skipped");
	CHECK(parse("wan = 100\n") == 0 && g.n == 1 &&
	          strcmp(g.items[0].key, "wan") == 0 &&
	          strcmp(g.items[0].value, "100") == 0 && g.items[0].block == -1 &&
	          !g.items[0].quoted && g.items[0].line == 1,
	      "a top-level pair");
	CHECK(parse("a=1 # trailing\n") == 0 && strcmp(g.items[0].value, "1") == 0,
	      "a trailing comment is dropped");
	CHECK(parse("dns = 1.1.1.1, 9.9.9.9\n") == 0 &&
	          strcmp(g.items[0].value, "1.1.1.1, 9.9.9.9") == 0,
	      "a bare value keeps its inner spaces");
	CHECK(parse("a = 1") == 0 && g.n == 1, "the last line needs no newline");
	CHECK(parse("a = 1\r\n") == 0 && strcmp(g.items[0].value, "1") == 0,
	      "CRLF line endings");
}

static void test_blocks(void) {
	CHECK(parse("match {\n interface = tnd0\n}\napply {\n dns = none\n}\n") ==
	              0 &&
	          g.nblocks == 2 && g.n == 2 && g.items[0].block == 0 &&
	          g.items[1].block == 1 && strcmp(g.blocks[1].name, "apply") == 0,
	      "two blocks with their items");
	CHECK(parse("vpn {\n}\n") == 0 && g.nblocks == 1 && g.n == 0,
	      "an empty block");
	CHECK(parse("a {\n b {\n }\n}\n") < 0 && g.err_line == 2,
	      "nested blocks are rejected");
	CHECK(parse("a {\n x = 1\n") < 0 && g.err_line == 1,
	      "an unclosed block is reported at its opening line");
	CHECK(parse("}\n") < 0, "a closing brace without a block");
	CHECK(parse("a { x = 1 }\n") < 0, "a block on a single line is rejected");
	CHECK(parse("eth0.100 {\n}\n") == 0 &&
	          strcmp(g.blocks[0].name, "eth0.100") == 0,
	      "dots in block names");
}

static void test_quoted(void) {
	CHECK(parse("ssid = \"Caf\xc3\xa9 Libre #2\"\n") == 0 &&
	          g.items[0].quoted &&
	          strcmp(g.items[0].value, "Caf\xc3\xa9 Libre #2") == 0,
	      "a quoted value keeps spaces and #");
	CHECK(parse("p = \"a\\\"b\\\\c\"\n") == 0 &&
	          strcmp(g.items[0].value, "a\"b\\c") == 0,
	      "escaped quote and backslash");
	CHECK(parse("p = \"\\x41\\x7e\"\n") == 0 &&
	          strcmp(g.items[0].value, "A~") == 0 && g.items[0].len == 2,
	      "hex escapes");
	CHECK(parse("p = \"\"\n") == 0 && g.items[0].len == 0,
	      "an empty quoted value parses");
	CHECK(parse("p = \"x\" # c\n") == 0, "a comment after the closing quote");
	CHECK(parse("p = \"\\x00\"\n") < 0, "\\x00 is rejected");
	CHECK(parse("p = \"\\n\"\n") < 0, "unknown escapes are rejected");
	CHECK(parse("p = \"\\x4\"\n") < 0, "a short hex escape is rejected");
	CHECK(parse("p = \"abc\n") < 0, "an unterminated quote is rejected");
	CHECK(parse("p = \"a\" b\n") < 0, "text after the closing quote");
}

static void test_errors(void) {
	CHECK(parse("a\n") < 0 && g.err_line == 1, "a name without '='");
	CHECK(parse("a =\n") < 0, "a missing value");
	CHECK(parse("a = # nothing\n") < 0, "a value that is only a comment");
	CHECK(parse("a = b\"c\n") < 0, "a stray quote in a bare value");
	CHECK(parse("= 1\n") < 0, "a missing name");
	CHECK(parse("x = 1\ny = 2\n%% = 3\n") < 0 && g.err_line == 3,
	      "the error line is reported");

	char nul[] = "a = 1\0b\n";
	CHECK(gr_parse(nul, sizeof(nul) - 1, &g) < 0, "a NUL byte is rejected");

	static char big[GR_LINE_MAX + 32];
	memcpy(big, "a = ", 4);
	memset(big + 4, 'x', sizeof(big) - 6);
	big[sizeof(big) - 2] = '\n';
	CHECK(parse(big) < 0, "a line too long is rejected");

	static char longval[GR_VALUE_MAX + 16];
	memcpy(longval, "a = \"", 5);
	memset(longval + 5, 'x', GR_VALUE_MAX);
	memcpy(longval + 5 + GR_VALUE_MAX, "\"\n", 3);
	CHECK(parse(longval) < 0, "a value too long is rejected");

	static char many[GR_ITEMS_MAX * 8 + 16];
	many[0] = '\0';
	for (int i = 0; i <= GR_ITEMS_MAX; i++)
		strcat(many, "k = v\n");
	CHECK(parse(many) < 0, "too many lines are rejected");
}

int main(void) {
	test_basic();
	test_blocks();
	test_quoted();
	test_errors();
	return test_summary();
}
