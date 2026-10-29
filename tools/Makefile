# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -O2 -Wall -Wextra -Werror -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

all: csfcat ../build/eventloop

csfcat: csfcat.c ../lib/coarse_stat_fs.h ../include/uapi/coarse_stat_fs.h
	$(CC) $(CFLAGS) -I../include -pthread $(LDFLAGS) -o $@ $<

../build/eventloop: ../examples/eventloop.c ../lib/coarse_stat_fs.h ../include/uapi/coarse_stat_fs.h
	@mkdir -p ../build
	$(CC) $(CFLAGS) -I../include -pthread $(LDFLAGS) -o $@ $<

.PHONY: all clean
clean:
	rm -f csfcat ../build/eventloop
