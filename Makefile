## Compilation

CC = gcc
VERSION = $(shell grep '^version=' metapkg | cut -d= -f2)

TUNDRA_IP_PATH ?= /usr/bin/tundra-ip
TUNDRA_DHCPC_PATH ?= /usr/bin/tundra-dhcpc
TUNDRA_BLUEICE_ARP_PATH ?= /usr/bin/tundra-blueice-arp
TUNDRA_WPA_SUPPLICANT_PATH ?= /usr/bin/wpa_supplicant
TUNDRA_NETWORKD_CONF_DIR ?= /etc/tundra/network
TUNDRA_NETWORKD_RUN_DIR ?= /run/tundra/network
TUNDRA_NETWORKD_SOCKET ?= /run/tundra/network.socket
TUNDRA_NETWORKD_SYSFS ?= /sys/class/net

TUNDRA_STD  = -std=c11
TUNDRA_DEFS = -D_GNU_SOURCE -DTUNDRA_NETWORKD_VERSION=\"$(VERSION)\" \
              -DTUNDRA_IP_PATH=\"$(TUNDRA_IP_PATH)\" \
              -DTUNDRA_DHCPC_PATH=\"$(TUNDRA_DHCPC_PATH)\" \
              -DTUNDRA_BLUEICE_ARP_PATH=\"$(TUNDRA_BLUEICE_ARP_PATH)\" \
              -DTUNDRA_WPA_SUPPLICANT_PATH=\"$(TUNDRA_WPA_SUPPLICANT_PATH)\" \
              -DTUNDRA_NETWORKD_CONF_DIR=\"$(TUNDRA_NETWORKD_CONF_DIR)\" \
              -DTUNDRA_NETWORKD_RUN_DIR=\"$(TUNDRA_NETWORKD_RUN_DIR)\" \
              -DTUNDRA_NETWORKD_SOCKET=\"$(TUNDRA_NETWORKD_SOCKET)\" \
              -DTUNDRA_NETWORKD_SYSFS=\"$(TUNDRA_NETWORKD_SYSFS)\"
TUNDRA_INC  = -Isrc -Isrc/conf -Isrc/net

CFLAGS ?= -Wall -Wextra -O2

TARGET = tundra-networkd

BUILD = build
SRCS = $(wildcard src/*.c src/conf/*.c src/net/*.c)
HDRS = $(wildcard src/*.h src/conf/*.h src/net/*.h)
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS = $(OBJS:.o=.d)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CPPFLAGS) $(CFLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) -MMD -MP -c $< -o $@

-include $(DEPS)

$(OBJS): Makefile metapkg

clean:
	rm -rf $(BUILD) $(TARGET)


## Tests

TEST_BUILD = $(BUILD)/test
TEST_BIN = $(TEST_BUILD)/tundra-networkd
TEST_TOOLS = $(patsubst tests/tools/%.c,$(TEST_BUILD)/tools/%,$(wildcard tests/tools/*.c))
TEST_DEFS = -D_GNU_SOURCE -DTUNDRA_NETWORKD_VERSION=\"$(VERSION)-test\" \
            -DTUNDRA_IP_PATH=\"$(CURDIR)/tests/tools/tip-wrap.sh\" \
            -DTUNDRA_DHCPC_PATH=\"$(CURDIR)/$(TEST_BUILD)/tools/dhcpc-fake\" \
            -DTUNDRA_BLUEICE_ARP_PATH=\"$(CURDIR)/$(TEST_BUILD)/tools/biarp-fake\" \
            -DTUNDRA_WPA_SUPPLICANT_PATH=\"$(CURDIR)/tests/tools/wpa-wrap.sh\" \
            -DTUNDRA_NETWORKD_CONF_DIR=\"$(CURDIR)/$(TEST_BUILD)/conf\" \
            -DTUNDRA_NETWORKD_RUN_DIR=\"$(CURDIR)/$(TEST_BUILD)/run\" \
            -DTUNDRA_NETWORKD_SOCKET=\"/run/tundra-test/network.socket\" \
            -DTUNDRA_NETWORKD_SYSFS=\"$(CURDIR)/$(TEST_BUILD)/sys\"

$(TEST_BIN): FORCE
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(TUNDRA_INC) $(TEST_DEFS) -o $@ $(SRCS)

$(TEST_BUILD)/tools/%: tests/tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) -D_GNU_SOURCE -o $@ $<

UNIT_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer
UNIT_TESTS = $(patsubst tests/unit/%.c,$(TEST_BUILD)/unit/%,$(wildcard tests/unit/*.c))

$(TEST_BUILD)/unit/%: tests/unit/%.c tests/unit/test.h $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) -Itests/unit \
		$(TEST_DEFS) -o $@ $<

check-unit: $(UNIT_TESTS)
	@rc=0; for t in $(UNIT_TESTS); do echo "# $$t"; $$t || rc=1; done; exit $$rc

check: check-unit $(TEST_BIN) $(TEST_TOOLS)
	@sh tests/net/run.sh

FORCE:

.PHONY: install uninstall clean check check-unit FORCE


## Installation

PREFIX ?= /usr
SYSCONFDIR ?= /etc
UNITDIR ?= $(PREFIX)/lib/systemd/system
DESTDIR ?=

CONF_FILES = 000-template.tnc 999-dhcp.tnc

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/$(TARGET)/LICENSE
	install -Dm644 data/000-template.tnc \
		$(DESTDIR)$(PREFIX)/share/tundra/network/000-template.tnc
	install -d $(DESTDIR)$(UNITDIR)
	sed 's|@BINDIR@|$(PREFIX)/bin|' data/tundra-networkd.service.in \
		>$(DESTDIR)$(UNITDIR)/tundra-networkd.service
	chmod 644 $(DESTDIR)$(UNITDIR)/tundra-networkd.service
	install -Dm644 data/tundra-networkd.sysusers \
		$(DESTDIR)$(PREFIX)/lib/sysusers.d/tundra-networkd.conf
	for f in $(CONF_FILES); do \
		[ -e $(DESTDIR)$(SYSCONFDIR)/tundra/network/$$f ] || \
		install -Dm644 data/$$f $(DESTDIR)$(SYSCONFDIR)/tundra/network/$$f; \
	done

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/tundra/network/000-template.tnc
	rm -f $(DESTDIR)$(UNITDIR)/tundra-networkd.service
	rm -f $(DESTDIR)$(PREFIX)/lib/sysusers.d/tundra-networkd.conf
