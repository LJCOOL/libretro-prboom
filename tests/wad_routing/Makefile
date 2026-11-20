# Routing lane: which IWAD, -file, and -deh arguments retro_load_game
# hands the engine for a given content path.  Each case builds a small
# card layout in a temp dir (real Freedoom IWADs, synthetic PWADs and
# playlists), loads it in a child process, and reads the
# "retro_load_game: argv:" log line the core emits before engine setup.
#
#   make -C tests/wad_routing check FREEDOOM_DIR=/abs/dir
#
# FREEDOOM_DIR must hold freedoom1.wad and freedoom2.wad; CI fetches
# Freedoom.  SAN= builds the harness under a sanitizer, to be paired with
# a core built the same way.

CORE        ?= $(firstword $(wildcard ../../prboom_libretro.so ../../prboom_libretro.dylib))
FREEDOOM_DIR ?= fd
RA          := ../..
CFLAGS      ?= -O1 -g -Wall -W
CFLAGS      += -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS      += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: wad_routing_test

wad_routing_test: wad_routing_test.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

check: all
	@test -f $(FREEDOOM_DIR)/freedoom1.wad -a -f $(FREEDOOM_DIR)/freedoom2.wad || \
	   { echo "no Freedoom IWADs in $(FREEDOOM_DIR); pass FREEDOOM_DIR=<dir>"; exit 2; }
	./wad_routing_test $(CORE) $(abspath $(FREEDOOM_DIR))

clean:
	rm -f wad_routing_test

.PHONY: all check clean
