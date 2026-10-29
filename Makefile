# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
CHECKPATCH ?= $(KDIR)/scripts/checkpatch.pl

KMOD_SRC := kmod/main.c kmod/cpu.c kmod/thread.c kmod/coarse_stat_fs.h kmod/compat.h include/uapi/coarse_stat_fs.h
USER_C := $(wildcard tools/*.c tests/*.c examples/*.c)
C_SRC := $(KMOD_SRC) lib/coarse_stat_fs.h tests/t.h $(USER_C)
SH_SRC := $(wildcard tests/*.sh tests/debian/*.sh)

all: ## build module, tools and tests
	$(MAKE) -C kmod KDIR=$(KDIR)
	$(MAKE) -C tools
	$(MAKE) -C tests

check: all ## run tests in a VM on the host kernel
	tests/vm.sh

bench: all ## run benchmarks in a VM
	VM_DEBUG=0 tests/vm.sh bench

SUITE ?= trixie
debian: ## run tests on a Debian kernel (SUITE=bookworm|trixie|sid)
	tests/debian/run.sh $(SUITE)

format: ## clang-format in place
	clang-format -i $(C_SRC)

format-check: ## check clang-format
	clang-format --dry-run --Werror $(C_SRC)

compdb: compdb-user compdb-kmod ## compile_commands.json for clangd/clang-tidy

compdb-user:
	bear --output compile_commands.json -- $(MAKE) -s -B -C tests
	bear --append --output compile_commands.json -- \
		$(MAKE) -s -B -C tools csfcat ../build/eventloop

compdb-kmod:
	python3 -I scripts/kmod-compdb.py $(KDIR)

compile_commands.json:
	$(MAKE) compdb-user

kmod/compile_commands.json:
	$(MAKE) compdb-kmod

tidy: tidy-user tidy-kmod ## run clang-tidy

tidy-user: compile_commands.json
	clang-tidy -p . --quiet $(USER_C)

tidy-kmod: kmod/compile_commands.json
	clang-tidy -p kmod --quiet $(filter %.c,$(KMOD_SRC))

# compat.h is exempt: version checks are its whole purpose, which
# checkpatch (written for in-tree code) always flags.
checkpatch: ## run checkpatch on the module
	$(CHECKPATCH) --no-tree --terse -q -f $(filter-out kmod/compat.h,$(KMOD_SRC))

shellcheck: ## run bash -n and shellcheck
	for f in $(SH_SRC) scripts/pre-commit; do bash -n $$f || exit 1; done
	if command -v shellcheck >/dev/null; then shellcheck $(SH_SRC) scripts/pre-commit; \
	else echo "shellcheck not installed; ran bash -n only"; fi

lint: format-check checkpatch shellcheck tidy ## all static checks and a W=1 build
	$(MAKE) -s -C kmod KDIR=$(KDIR) W=1

help: ## list targets
	@grep -hE '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | sed 's/:.*## /\t/' | expand -t 16

clean: ## remove build output
	$(MAKE) -C kmod clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
	rm -rf build compile_commands.json kmod/compile_commands.json

.PHONY: all check bench debian format format-check compdb compdb-user compdb-kmod tidy \
	tidy-user tidy-kmod checkpatch shellcheck lint help clean
