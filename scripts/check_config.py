#!/usr/bin/env python3
"""Static consistency check: every config key written by np_config_save
must have a load branch in np_config_load, and vice versa.

Line formats checked (src/rapideditor.c):
  load:  strcmp(key, "<name>")           / strncmp(key, "<prefix>", n)
  save:  fprintf-format literals         "<name>=%d\\n"  (split on \\n)

Keys written only by other writers (draft files, sessions) are listed in
EXTRA_OK below with a pointer to their reader, so future drift in the
main config is what actually fails.
"""
import re
import sys

SRC = __file__.rsplit("scripts", 1)[0] + "src/rapideditor.c"

# Keys that appear in save-side literals but are read by a dedicated
# parser elsewhere (not np_config_load). Keep this list short.
EXTRA_OK = {"path", "enc", "crlf"}  # draft file header; read in np_load_draft

load_exact = set(re.findall(r'strcmp\(key,\s*"([a-z_0-9%]+)"\)', open(SRC, encoding="utf-8", errors="replace").read()))
src = open(SRC, encoding="utf-8", errors="replace").read()

save = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    if "=" not in lit:
        continue
    if not re.search(r"%(?:d|s|u|f)", lit):
        continue
    for part in lit.split("\\n"):
        m = re.match(r"^([a-z_0-9]+)=", part)
        if m:
            save.add(m.group(1))

missing_load = sorted(save - load_exact - EXTRA_OK)
missing_save = sorted(load_exact - save)

fails = 0
if missing_load:
    print("FAIL keys saved but never loaded:", ", ".join(missing_load))
    fails += 1
if missing_save:
    print("FAIL keys loaded but never saved:", ", ".join(missing_save))
    fails += 1
if not fails:
    print(f"ok  - config keys consistent ({len(load_exact)} load / {len(save)} save)")
sys.exit(fails)
