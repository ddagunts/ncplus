#!/usr/bin/env python3
"""Compare two tools/dwarf_layout.py dumps: print types whose layouts differ, plus counts.

  dwarf_layout_diff.py ref.txt ours.txt [--only-common]
Types present on one side only are listed by name (most are kernel or env-internal types).
"""
import difflib
import sys


def blocks(path):
    out = {}
    for b in open(path).read().strip().split("\n\n"):
        lines = b.splitlines()
        key = lines[0].rsplit(" size ", 1)[0]
        out.setdefault(key, []).append(b)
    return out


a, b = blocks(sys.argv[1]), blocks(sys.argv[2])
common = sorted(set(a) & set(b))
same = 0
for k in common:
    if a[k][0] == b[k][0]:
        same += 1
        continue
    print("".join(difflib.unified_diff(a[k][0].splitlines(True), b[k][0].splitlines(True),
                                       "ref", "ours", n=1)))
print("# %d common types, %d identical, %d differ" % (len(common), same, len(common) - same))
if "--only-common" not in sys.argv:
    print("# only in ref:", " ".join(sorted(set(a) - set(b))))
    print("# only in ours:", " ".join(sorted(set(b) - set(a))))
