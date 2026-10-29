# SPDX-License-Identifier: GPL-2.0
OUT ?= ../build/tests
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
override CFLAGS += -I../include -pthread
TESTS := t_cpu t_thread t_life t_stress t_hold bench

all: $(addprefix $(OUT)/,$(TESTS))

$(OUT)/%: %.c t.h ../lib/coarse_stat_fs.h ../include/uapi/coarse_stat_fs.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
