# ARSync - rsync client for AmigaOS 3.x
#
#   make              host and Amiga builds of arsync and the unit tests
#   make test         run the unit tests natively on the host
#   make test-amiga   run the Amiga unit-test binary under vamos (68020)
#   make itest        integration tests against a local rsync daemon
#   make clean

HOST_CC  ?= cc
AMIGA_CC ?= /opt/amiga/bin/m68k-amigaos-gcc
VAMOS    ?= .venv/bin/vamos

WARN = -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS  = -std=gnu99 -O2 -g $(WARN) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
# libnix runtime: smaller binaries than the default newlib (22 KB vs 72 KB
# for arsync), no mathieeedoubbas.library dependency, and its argv handling
# works under vamos (newlib's comes back empty there).
AMIGA_RT = -mcrt=nix20 -m68020
# -Wno-pointer-sign: the NDK's STRPTR is unsigned char *.
AMIGA_CFLAGS = -std=gnu99 -O2 $(AMIGA_RT) $(WARN) -Wno-pointer-sign

CORE_SRC = src/wire.c src/amitime.c src/plat_common.c src/url.c src/options.c \
           src/net.c src/session.c src/digest.c src/auth.c src/flist.c \
           src/output.c src/client.c src/generator.c src/receiver.c
APP_SRC  = src/main.c
TEST_SRC = tests/test_main.c tests/test_wire.c tests/test_amitime.c tests/test_plat.c \
           tests/test_session.c tests/test_auth.c tests/test_flist.c \
           tests/test_pull.c

HOST_DIR  = build/host
AMIGA_DIR = build/amiga

obj = $(addprefix $(1)/,$(notdir $(2:.c=.o)))

HOST_CORE  = $(call obj,$(HOST_DIR),$(CORE_SRC) src/plat_posix.c)
AMIGA_CORE = $(call obj,$(AMIGA_DIR),$(CORE_SRC) src/plat_amiga.c)

vpath %.c src tests

.PHONY: all host amiga test test-amiga itest clean

all: host amiga

host: $(HOST_DIR)/arsync $(HOST_DIR)/arsync-test
amiga: $(AMIGA_DIR)/arsync $(AMIGA_DIR)/arsync-test

$(HOST_DIR)/arsync: $(HOST_CORE) $(call obj,$(HOST_DIR),$(APP_SRC))
	$(HOST_CC) -o $@ $^

$(HOST_DIR)/arsync-test: $(HOST_CORE) $(call obj,$(HOST_DIR),$(TEST_SRC))
	$(HOST_CC) -o $@ $^

$(AMIGA_DIR)/arsync: $(AMIGA_CORE) $(call obj,$(AMIGA_DIR),$(APP_SRC))
	$(AMIGA_CC) $(AMIGA_RT) -o $@ $^

$(AMIGA_DIR)/arsync-test: $(AMIGA_CORE) $(call obj,$(AMIGA_DIR),$(TEST_SRC))
	$(AMIGA_CC) $(AMIGA_RT) -o $@ $^

$(HOST_DIR)/%.o: %.c | $(HOST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(AMIGA_DIR)/%.o: %.c | $(AMIGA_DIR)
	$(AMIGA_CC) $(AMIGA_CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(HOST_DIR) $(AMIGA_DIR):
	mkdir -p $@

# Tests run inside the build directory: test_plat.c makes a scratch dir in
# the current directory.
test: $(HOST_DIR)/arsync-test
	cd $(HOST_DIR) && ./arsync-test

test-amiga: $(AMIGA_DIR)/arsync-test
	@test -x $(VAMOS) || { echo "vamos not found: run tools/setup-vamos.sh"; exit 1; }
	cd $(AMIGA_DIR) && $(abspath $(VAMOS)) -C 68020 -m 8192 arsync-test

itest: $(HOST_DIR)/arsync
	tests/itest.sh $(HOST_DIR)/arsync

clean:
	rm -rf build

-include $(wildcard $(HOST_DIR)/*.d $(AMIGA_DIR)/*.d)
