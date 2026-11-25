#!/usr/bin/env python3
"""Recover a PowerVR DDK 1.x kernel module's bridge dispatch table: ioctl index -> handler.

  pvr_bridge_table.py OBJDUMP module.ko
Walks CommonBridgeInit and SetSGXDispatchTableEntry with a tiny register tracker: literal
loads (with their relocations), mov/add, stores into g_BridgeDispatchTable (when gcc
inlined SetDispatchTableEntry) and calls to _SetDispatchTableEntry(index, name, fn, name).
Prints "index handler" lines, sorted. Used to check that our pvrsrvkm.ko numbers its
bridge calls like LG's, which the TV's libsrv_um.so is built against.
"""
import re
import subprocess
import sys

objdump, ko = sys.argv[1], sys.argv[2]

# symbol table: index -> (name, value, section index); section symbols are named after
# their section, and gcc's local anchors (.LANCHORn) only make sense with their value
symtab, secnames = {}, {}
for line in subprocess.run(["readelf", "-sW", ko], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 7 and p[0].endswith(":") and p[0][:-1].isdigit():
        symtab[int(p[0][:-1])] = (p[7] if len(p) > 7 else "", int(p[1], 16), p[6])
for line in subprocess.run(["readelf", "-SW", ko], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
    if m:
        secnames[m.group(1)] = m.group(2)
for i, (n, v, sh) in list(symtab.items()):
    if not n and sh in secnames:
        symtab[i] = (secnames[sh], v, sh)

# relocations in .rel.text: address -> (symbol name, symbol value, section)
relsym = {}
cursec = None
for line in subprocess.run(["readelf", "-rW", ko], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"Relocation section '(\S+)'", line)
    if m:
        cursec = m.group(1)
        continue
    p = line.split()
    if cursec == ".rel.text" and len(p) >= 2 and re.match(r"[0-9a-f]{8}$", p[0]):
        relsym[int(p[0], 16)] = symtab[int(p[1], 16) >> 8]

tab = [v for v in symtab.values() if v[0] == "g_BridgeDispatchTable"][0]
tab_sec, tab_off = tab[2], tab[1]

dis = subprocess.run([objdump, "-dr", "--no-show-raw-insn", ko], capture_output=True,
                     text=True).stdout.splitlines()

# literal words and relocations by address
words, relocs = {}, {}
cur = None
for line in dis:
    m = re.match(r"\s+([0-9a-f]+):\s+R_ARM_(\w+)\s+(\S+)", line)
    if m:
        relocs[int(m.group(1), 16)] = (m.group(2), m.group(3))
        continue
    m = re.match(r"\s+([0-9a-f]+):\s+\.word\s+0x([0-9a-f]+)", line)
    if m:
        words[int(m.group(1), 16)] = int(m.group(2), 16)


def func_lines(name):
    out, on = [], False
    for line in dis:
        if re.match(r"[0-9a-f]+ <%s>:" % re.escape(name), line):
            on = True
            continue
        if on and re.match(r"[0-9a-f]+ <", line):
            break
        if on:
            out.append(line)
    return out


def symval(addr, addend):
    """("sym", name, section, offset-in-section) for a relocated value"""
    n, v, sh = relsym[addr]
    return ("sym", n, sh, v + addend)


def literal(addr):
    w = words.get(addr)
    if addr in relsym:
        return symval(addr, w or 0)
    return ("const", w)


table = {}


def run(name):
    regs = {}
    for line in func_lines(name):
        m = re.match(r"\s+([0-9a-f]+):\s+(\w+)\s*(.*)", line)
        if not m or "R_ARM" in line:
            continue
        op, args = m.group(2), m.group(3).split(";")[0].strip()
        a = [x.strip() for x in re.split(r",(?![^\[]*\])", args)] if args else []
        addr = int(m.group(1), 16)
        if op in ("movw", "movt") and addr in relsym:
            if op == "movw":
                regs[a[0]] = symval(addr, 0)
            continue
        if op == "ldr" and re.match(r"\[pc, #-?\d+\]", a[1]):
            ref = re.search(r";\s*([0-9a-f]+)", m.group(3))
            regs[a[0]] = literal(int(ref.group(1), 16))
        elif op == "mov" and a[1].startswith("#"):
            regs[a[0]] = ("const", int(a[1][1:], 0))
        elif op == "mov":
            regs[a[0]] = regs.get(a[1])
        elif op == "add" and len(a) == 3 and a[2].startswith("#") and regs.get(a[1]) \
                and regs[a[1]][0] == "sym":
            k, n, sh, v = regs[a[1]]
            regs[a[0]] = (k, n, sh, v + int(a[2][1:], 0))
        elif op == "str":
            mm = re.match(r"\[(\w+)(?:, #(-?\d+))?\]$", a[1])
            base = regs.get(mm.group(1)) if mm else None
            if base and base[0] == "sym" and base[2] == tab_sec:
                idx = (base[3] + int(mm.group(2) or 0) - tab_off) // 4
                val = regs.get(a[0])
                table[idx] = val[1] if val and val[0] == "sym" else repr(val)
        elif op == "bl":
            target = re.search(r"<([^>+]+)", m.group(3))
            if target and target.group(1).split(".")[0] == "_SetDispatchTableEntry":
                i, f = regs.get("r0"), regs.get("r2")
                if i and i[0] == "const":
                    table[i[1]] = f[1] if f and f[0] == "sym" else repr(f)
            for r in ("r0", "r1", "r2", "r3", "ip", "lr"):
                regs.pop(r, None)
        elif a and re.match(r"r\d+|ip|lr|sl|fp", a[0]) and op not in ("cmp", "tst", "push",
                                                                       "pop", "b", "bx"):
            regs.pop(a[0], None)


run("CommonBridgeInit")
run("SetSGXDispatchTableEntry")
for i in sorted(table):
    print(i, table[i])
