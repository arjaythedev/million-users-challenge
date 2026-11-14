CC=gcc
# Pass 15: gzip via vendored libdeflate (static). No system zlib (-lz) anymore.
LIBDEFLATE_VER=1.26
LIBDEFLATE_DIR=vendor/libdeflate-$(LIBDEFLATE_VER)
LIBDEFLATE_A=$(LIBDEFLATE_DIR)/libdeflate.a
# Pass 18: vendored picohttpparser (parse-in-place HTTP/1.x parser, static,
# pinned tarball + sha256 like libdeflate above; -I$(PICO_DIR) in CFLAGS).
PICO_SHA=dd573d8fa9264c646a400e96ce9f07dea6f156e4
PICO_DIR=vendor/picohttpparser-$(PICO_SHA)
PICO_C=$(PICO_DIR)/picohttpparser.c
PICO_H=$(PICO_DIR)/picohttpparser.h
# -march=x86-64-v3 (AVX/AVX2/BMI/FMA): valid on the Ryzen build host and on the
# Xeon 8260-class bench box (Cascade Lake, Haswell+ ISA). Falls back to generic
# -O3 only if a future bench CPU predates it (it does not).
# Portable libpq flags: Ubuntu 24.04 puts libpq-fe.h in /usr/include/postgresql
# (needs -I/usr/include/postgresql); CachyOS/Arch puts it in /usr/include.
# pkg-config covers both; the hardcoded fallback covers a missing pkg-config.
PG_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null || echo -I/usr/include/postgresql)
PG_LDLIBS := $(shell pkg-config --libs libpq 2>/dev/null || echo -lpq)
CFLAGS=-O3 -march=x86-64-v3 -flto -Wall -Wextra -D_GNU_SOURCE -I$(LIBDEFLATE_DIR) -I$(PICO_DIR) $(PG_CFLAGS)
LDLIBS=-luring $(PG_LDLIBS) -lcrypto
TARGET=server
SRCS=server.c net.c http.c pg.c json.c jwt.c
OBJS=server.o net.o http.o pg.o json.o jwt.o pico.o
NPROC := $(shell nproc 2>/dev/null || echo 16)

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBDEFLATE_A)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBDEFLATE_A) $(LDLIBS)

# Vendor the static lib on demand (fetch + sha256 verify + extract + build);
# a no-op when libdeflate.a already exists. `make clean` keeps vendor/.
$(LIBDEFLATE_A):
	bash ./build-vendor.sh

# CI fresh-clone race: with `make -j`, %.o compiles (needs
# -Ivendor/libdeflate-*/libdeflate.h and -Ivendor/picohttpparser-*/) in
# parallel with the vendor extract above. Serialize objects after the
# vendored static lib + pico headers so a fresh clone (tarballs only, no
# extracted dirs) builds the headers first.
$(OBJS): $(LIBDEFLATE_A) $(PICO_H)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Pass 18: picohttpparser has no build system of its own in the tarball
# layout we need (upstream Makefile builds a test harness); compile the
# single translation unit straight into pico.o with our strict flags.
# The header/source are produced by build-vendor.sh (same rule as above).
$(PICO_C) $(PICO_H): $(LIBDEFLATE_A)
	@test -f $(PICO_C) -a -f $(PICO_H) || { echo "picohttpparser missing after build-vendor.sh" >&2; exit 1; }

pico.o: $(PICO_C) $(PICO_H)
	$(CC) $(CFLAGS) -c -o $@ $(PICO_C)

# Pass 12: optional PGO build (NOT default; CI and build.sh keep the plain
# target above). Instruments, runs the 5k mload profile pass
# (pgo-profile.sh: scratch DB on feed18, SIGTERM dump via the BUILD_PGO_GEN
# hook in server.c), drops the instrumented objects (keeping .gcda/.gcno),
# then rebuilds with -fprofile-use -fprofile-correction -DBUILD_PGO_GEN.
# -fprofile-correction tolerates torn counters from the 16 racing workers
# (documented for multithreaded profiles); -DBUILD_PGO_GEN is kept so main
# keeps an identical CFG. -Wl,-u,__gcov_dump pulls the dump routine out of
# the static libgcov.a (weak refs alone never link it). Validate with 42/42 +
# golden-body diff vs plain.
pgo:
	$(MAKE) clean
	$(MAKE) -j$(NPROC) CFLAGS="$(CFLAGS) -fprofile-generate -DBUILD_PGO_GEN -Wl,-u,__gcov_dump" $(TARGET)
	bash ./pgo-profile.sh
	rm -f $(OBJS)
	$(MAKE) -j$(NPROC) CFLAGS="$(CFLAGS) -fprofile-use -fprofile-correction -DBUILD_PGO_GEN" $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) *.gcda *.gcno

.PHONY: all clean pgo

# Pass 20: `make USE_EPOLL_RECV=1` forces the epoll recv path (bisection or
# kernels without RECV_MULTISHOT). Default is multishot recv with the epoll
# path kept as the runtime fallback. Both paths stay compiled either way.
ifdef USE_EPOLL_RECV
CFLAGS += -DUSE_EPOLL_RECV
endif
