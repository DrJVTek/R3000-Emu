# DevPSX common build config for R3000-Emu test programs.
# Reuses the PsyQ 4.7 + Nugget toolchain from nolibgs_hello_worlds.
#
# Usage: each test program defines TARGET and SRCS, then includes this file.
#   TARGET = my_test
#   SRCS = my_test.c \
#   include ../common.mk

TYPE = ps-exe

# Path to nolibgs_hello_worlds (contains thirdparty/nugget + psyq SDK)
NOLIBGS := E:/Projects/PSX/nolibgs_hello_worlds

THISDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# CRT0 startup + printf syscall wrapper
SRCS += $(NOLIBGS)/thirdparty/nugget/common/crt0/crt0.s
SRCS += $(NOLIBGS)/thirdparty/nugget/common/syscalls/printf.s

# SDK includes (PsyQ 4.7 headers)
CPPFLAGS += -I$(NOLIBGS)/psyq/include

# SDK libraries
LDFLAGS += -L$(NOLIBGS)/psyq/lib
LDFLAGS += -Wl,--start-group
LDFLAGS += -lapi
LDFLAGS += -lc
LDFLAGS += -lc2
LDFLAGS += -letc
LDFLAGS += -lgpu
LDFLAGS += -lgte
LDFLAGS += -lmath
LDFLAGS += -lpad
LDFLAGS += -lspu
LDFLAGS += -lcd
LDFLAGS += -Wl,--end-group

# Nugget build infrastructure (compiler flags, linker script, rules)
include $(NOLIBGS)/thirdparty/nugget/common.mk
