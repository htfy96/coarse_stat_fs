#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# kbuild's compile_commands.json carries gcc-only flags that clang rejects.
import json
import os
import re
import shlex
import subprocess
import sys

GCC_ONLY = re.compile(
    r"-fconserve-stack|-fdiagnostics-show-context=.*|-fmin-function-alignment=.*"
    r"|-fno-allow-store-data-races|-fzero-init-padding-bits=.*|-falign-jumps=.*"
    r"|-mpreferred-stack-boundary=.*|-mindirect-branch=.*|-mindirect-branch-register"
    r"|-mrecord-mcount|-fno-ipa-sra|-fsanitize=bounds-strict|-Werror=designated-init"
    r"|-Wimplicit-fallthrough=.*|-Wvla-larger-than=.*|-Wp,-MMD,.*")

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
kmod = os.path.join(root, "kmod")
kdir = sys.argv[1] if len(sys.argv) > 1 else f"/lib/modules/{os.uname().release}/build"
path = os.path.join(kmod, "compile_commands.json")
if os.path.exists(path):
    os.unlink(path)  # kbuild won't regenerate a file newer than its inputs
subprocess.run(["make", "-s", "-C", kdir, f"M={kmod}", "compile_commands.json"], check=True)

with open(path) as f:
    db = json.load(f)
for e in db:
    args = e.pop("arguments", None) or shlex.split(e.pop("command"))
    e["arguments"] = [a for a in args if not GCC_ONLY.fullmatch(a)]
    e["arguments"].insert(-1, "-Wno-unknown-warning-option")
with open(path, "w") as f:
    json.dump(db, f, indent=1)
print(f"wrote {path} ({len(db)} entries)")
