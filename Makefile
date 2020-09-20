# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2 -pedantic
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif

CFLAGS += -I.

.SUFFIXES: .c .o .so

.c.o:
	$(CC) $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

include deps/rules.mk

MODULE=librediscompress.so
OBJS=$(patsubst %.c,%.o,$(wildcard src/*.c))
LIBS=deps/zstd/lib/libzstd.a

module: deps/redis deps/zstd $(MODULE)
$(MODULE): $(OBJS)
	$(LD) -o $(MODULE) $(OBJS) $(SHOBJ_LDFLAGS) $(LIBS) -lc

.PHONY: all module clean
 
all: module

clean:
	rm -f $(OBJS) $(MODULE)
