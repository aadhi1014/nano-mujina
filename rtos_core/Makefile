# Standalone RT-Smart userspace build for rtos_core.
#
# Toolchain: a real musl RISC-V64 cross-toolchain is already extracted at
# toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/ (repo root, not
# on PATH by default). Don't run maix3_toolchain.sh or look for one
# elsewhere -- just run build_wsl.sh (from WSL), which sets PATH to it.
# This produces the SAME real musl build that's confirmed (2026-07-23) to
# run on the actual RT-Smart big core -- see docs/BUILD_NOTES.md.

# NOTE: plain `CC ?=` doesn't work here -- GNU Make's built-in implicit rules
# already define CC=cc, and that counts as "already set" for `?=` purposes,
# so it silently keeps using the host's native cc instead of the cross
# compiler. Only override when CC's origin is make's own built-in default.
ifeq ($(origin CC),default)
CC := riscv64-unknown-linux-musl-gcc
endif
OUT ?= build/rtos_core.elf

# -fms-extensions: sha256.h (real vendor header, avalon_mm_src/big/mmu/sha256.h)
# uses an anonymous member of a named typedef'd struct type
# ("hash_config_t;" with no member name inside a union) to let sha256.c
# access ctx->msg directly instead of ctx->config.msg. That's a
# Microsoft/GNU extension, not standard C11 anonymous-member syntax (which
# requires the nested type itself to be anonymous, not just the member) --
# GCC supports it but not by default. Enabling this rather than rewriting
# the vendor source's access pattern everywhere.
CFLAGS := -mcmodel=medany -march=rv64imafdcv -mabi=lp64d -fms-extensions -Wall -Wextra -O0 -gdwarf-2 -n --static
CPPFLAGS := -Iinclude -Iinclude/sdk
LDFLAGS := -T link.lds -n --static -Lvendor/sdk_libs -Lvendor/sdk_resource/lib
# --start-group/--end-group: libipcmsg.a (kd_ipcmsg_*) and libpthread have a
# circular static-link dependency (ipcmsg uses pthread_*, and our own code
# uses both), same grouping the K230 SDK's own sample Makefiles use.
#
# Links the SLAVE variant of libipcmsg.a (vendor/sdk_resource/lib, fetched
# from k230_sdk's src/common/cdk/user/component/ipcmsg/slave/lib), not the
# host variant in vendor/sdk_libs/ -- rtos_core runs on the physical RT-Smart
# ("slave") hardware side, matching avalon_mm_src/big/CMakeLists.txt's own
# link_directories(.../ipcmsg/slave/lib) for the real asic_miner build. Using
# the host variant here was a live bug: see docs/BUILD_NOTES.md.
LDLIBS := -Wl,--start-group -l:libipcmsg_slave.a -lpthread -lm -Wl,--end-group

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

.PHONY: all clean

all: $(OUT)

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LDLIBS)

clean:
	rm -rf build
