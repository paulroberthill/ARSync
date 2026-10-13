# ARSync - rsync client for AmigaOS 3.x
#
#   make              host and Amiga builds of arsync and the unit tests
#   make test         run the unit tests natively on the host
#   make test-amiga   run the Amiga unit-test binary under vamos (68020)
#   make itest        integration tests against a local rsync daemon
#   make release      build/release/arsync-VERSION.lha (stripped Amiga binary + docs)
#   make clean

HOST_CC  ?= cc
AMIGA_CC ?= /opt/amiga/bin/m68k-amigaos-gcc
AMIGA_STRIP ?= /opt/amiga/bin/m68k-amigaos-strip
AMIGA_AR ?= /opt/amiga/bin/m68k-amigaos-ar
VAMOS    ?= .venv/bin/vamos

WARN = -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS  = -std=gnu99 -O2 -g $(WARN) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
# libnix runtime: smaller binaries than the default newlib (22 KB vs 72 KB
# for arsync), no mathieeedoubbas.library dependency, and its argv handling
# works under vamos (newlib's comes back empty there).
AMIGA_RT = -mcrt=nix20 -m68020
# -Wno-pointer-sign: the NDK's STRPTR is unsigned char *.
AMIGA_CFLAGS = -std=gnu99 -O2 $(AMIGA_RT) $(WARN) -Wno-pointer-sign -I$(ZLIB_DIR)
# zlib for -z: the system's on the host; on the Amiga, zlib 1.3.1 built from
# third_party/zlib. Not the toolchain's /opt/amiga/lib/libz.a: GCC 6.5 -O2
# miscompiles trees.c (stray byte at the end of each deflate flush; zlib's
# own test/example.c fails under vamos), so trees.c is built at -O1 here.
# Also the driver puts -L/opt/amiga/lib before any user -L, so -lz would
# always find the toolchain's copy; link the archive by path.
ZLIB_DIR   = third_party/zlib
ZLIB_SRC   = adler32.c crc32.c deflate.c inffast.c inflate.c inftrees.c trees.c zutil.c
ZLIB_CFLAGS = -O2 $(AMIGA_RT) -I$(ZLIB_DIR)
HOST_LIBS  = -lz
AMIGA_LIBS = $(AMIGA_DIR)/zlib/libz.a

CORE_SRC = src/wire.c src/amitime.c src/plat_common.c src/url.c src/options.c \
           src/net.c src/session.c src/digest.c src/auth.c src/flist.c \
           src/output.c src/client.c src/generator.c src/receiver.c \
           src/checksum.c src/fsmap.c src/progress.c src/scan.c src/match.c \
           src/push.c src/filter.c src/token.c src/delete.c
APP_SRC  = src/main.c
TEST_SRC = tests/test_main.c tests/test_wire.c tests/test_amitime.c tests/test_plat.c \
           tests/test_session.c tests/test_auth.c tests/test_flist.c \
           tests/test_pull.c tests/test_checksum.c \
           tests/test_fsmap.c tests/test_push.c tests/test_filter.c

HOST_DIR  = build/host
AMIGA_DIR = build/amiga

obj = $(addprefix $(1)/,$(notdir $(2:.c=.o)))

HOST_CORE  = $(call obj,$(HOST_DIR),$(CORE_SRC) src/plat_posix.c)
AMIGA_CORE = $(call obj,$(AMIGA_DIR),$(CORE_SRC) src/plat_amiga.c)

vpath %.c src tests

.PHONY: all host amiga test test-amiga itest release clean

all: host amiga

host: $(HOST_DIR)/arsync $(HOST_DIR)/arsync-test
amiga: $(AMIGA_DIR)/arsync $(AMIGA_DIR)/arsync-test

$(HOST_DIR)/arsync: $(HOST_CORE) $(call obj,$(HOST_DIR),$(APP_SRC))
	$(HOST_CC) -o $@ $^ $(HOST_LIBS)

$(HOST_DIR)/arsync-test: $(HOST_CORE) $(call obj,$(HOST_DIR),$(TEST_SRC))
	$(HOST_CC) -o $@ $^ $(HOST_LIBS)

$(AMIGA_DIR)/arsync: $(AMIGA_CORE) $(call obj,$(AMIGA_DIR),$(APP_SRC)) $(AMIGA_LIBS)
	$(AMIGA_CC) $(AMIGA_RT) -o $@ $^

$(AMIGA_DIR)/arsync-test: $(AMIGA_CORE) $(call obj,$(AMIGA_DIR),$(TEST_SRC)) $(AMIGA_LIBS)
	$(AMIGA_CC) $(AMIGA_RT) -o $@ $^

$(HOST_DIR)/%.o: %.c | $(HOST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(AMIGA_DIR)/%.o: %.c | $(AMIGA_DIR)
	$(AMIGA_CC) $(AMIGA_CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(HOST_DIR) $(AMIGA_DIR) $(AMIGA_DIR)/zlib:
	mkdir -p $@

$(AMIGA_DIR)/zlib/libz.a: $(addprefix $(AMIGA_DIR)/zlib/,$(ZLIB_SRC:.c=.o))
	rm -f $@
	$(AMIGA_AR) rcs $@ $^

$(AMIGA_DIR)/zlib/%.o: $(ZLIB_DIR)/%.c | $(AMIGA_DIR)/zlib
	$(AMIGA_CC) $(ZLIB_CFLAGS) -MMD -MP -c -o $@ $<

$(AMIGA_DIR)/zlib/trees.o: ZLIB_CFLAGS += -O1

# Tests run inside the build directory: test_plat.c makes a scratch dir in
# the current directory.
test: $(HOST_DIR)/arsync-test
	cd $(HOST_DIR) && ./arsync-test

test-amiga: $(AMIGA_DIR)/arsync-test
	@test -x $(VAMOS) || { echo "vamos not found: run tools/setup-vamos.sh"; exit 1; }
	cd $(AMIGA_DIR) && $(abspath $(VAMOS)) -C 68020 -m 8192 arsync-test

itest: $(HOST_DIR)/arsync
	tests/itest.sh $(HOST_DIR)/arsync

VERSION = $(shell sed -n 's/^\#define ARSYNC_VERSION "\(.*\)"/\1/p' src/arsync.h)
RELEASE_DIR = build/release/arsync

release: $(AMIGA_DIR)/arsync
	rm -rf build/release
	mkdir -p $(RELEASE_DIR)
	$(AMIGA_STRIP) -o $(RELEASE_DIR)/arsync $(AMIGA_DIR)/arsync
	sed 's/@VERSION@/$(VERSION)/' dist/arsync.readme > $(RELEASE_DIR)/arsync.readme
	@# Aminet wants the readme next to the archive too, with the same name.
	cp $(RELEASE_DIR)/arsync.readme build/release/arsync-$(VERSION).readme
	cp README.md LICENSE $(RELEASE_DIR)/
	@# Aminet wants .lha; Lhasa (often installed as "lha") can't create
	@# archives, so fall back to .tar.gz when no LHA archiver is found.
	@if lha 2>&1 | grep -q '^ *a '; then \
	    (cd build/release && lha aq arsync-$(VERSION).lha arsync) && ls -l build/release/arsync-$(VERSION).lha; \
	else \
	    tar -C build/release -czf build/release/arsync-$(VERSION).tar.gz arsync && ls -l build/release/arsync-$(VERSION).tar.gz; \
	    echo "note: no LHA archiver that can create archives (e.g. the 'lha' from jca02266/lha); made a .tar.gz instead"; \
	fi

clean:
	rm -rf build

-include $(wildcard $(HOST_DIR)/*.d $(AMIGA_DIR)/*.d $(AMIGA_DIR)/zlib/*.d)
