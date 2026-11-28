#!/usr/bin/env python3
"""Check that the ESP-IDF component pins match the committed lock file.

Why: a range such as "^1.0.0" lets each fresh build pick whichever 1.x was
newest. The mDNS component 1.11.3 made the test unit's link slow; 1.15.1 did
not. So every direct component must be pinned to one exact version, and
dependencies.lock (committed) must resolve it to that same version.

Usage: python3 tools/check-component-pins.py [repo-root]
Exit status 0 = consistent, 1 = a pin is inexact, missing or disagrees with the lock.
Standard library only, so it runs on the CI runner and on the Windows PC alike.
"""
import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
manifest = os.path.join(root, "main", "idf_component.yml")
lockfile = os.path.join(root, "dependencies.lock")

errors = []

# Direct pins: lines like   espressif/mdns: "1.15.1"
pins = {}
with open(manifest, encoding="utf-8") as f:
    for line in f:
        m = re.match(r'^\s*(espressif/[\w.-]+):\s*"([^"]*)"', line)
        if not m:
            continue
        name, spec = m.group(1), m.group(2)
        if not re.fullmatch(r"\d+\.\d+\.\d+", spec):
            errors.append(f"{name}: '{spec}' is not an exact version (use x.y.z)")
        pins[name] = spec

if not pins:
    errors.append("no espressif/* components found in main/idf_component.yml")

# Resolved versions: a 2-space indented "espressif/x:" block with a 4-space
# "version:" line under it.
resolved = {}
if not os.path.exists(lockfile):
    errors.append("dependencies.lock is missing (it must be committed)")
else:
    current = None
    with open(lockfile, encoding="utf-8") as f:
        for line in f:
            head = re.match(r"^  (espressif/[\w.-]+):\s*$", line)
            if head:
                current = head.group(1)
                continue
            if re.match(r"^  \S", line):
                current = None
                continue
            ver = re.match(r"^    version:\s*(\S+)\s*$", line)
            if ver and current:
                resolved[current] = ver.group(1).strip("'\"")

for name, spec in pins.items():
    got = resolved.get(name)
    if got is None:
        errors.append(f"{name}: pinned to {spec} but absent from dependencies.lock")
    elif got != spec:
        errors.append(f"{name}: pinned to {spec} but dependencies.lock resolves {got}")

if errors:
    print("component pins are not consistent:")
    for e in errors:
        print("  - " + e)
    print("Fix main/idf_component.yml or regenerate dependencies.lock, then retest the link.")
    sys.exit(1)

print("component pins OK:", ", ".join(f"{n} {v}" for n, v in sorted(pins.items())))
