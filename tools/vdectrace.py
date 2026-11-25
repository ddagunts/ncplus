#!/usr/bin/env python3
"""vdectrace.py trace.txt [--pic N] [--diff]
Read a /proc/ncvdec log (src/kmod/ncvdec.c) and split it at the decode start (0xf0030238 = 1).
Default: one line per start. --pic N: every access that leads to start N. --diff: per start, the
registers whose last written value changed since the previous one."""
import argparse, re, sys

KICK = 0xf0030238
BLOCKS = {0xf0002: "VLD", 0xf0003: "VSYS", 0xf000a: "MC", 0xf000d: "CKGEN", 0xf002f: "PP",
          0xf0030: "AVLD", 0xf0031: "AMV"}

def name(a):
    b = BLOCKS.get(a >> 12, "?")
    if b == "VLD" and a & 0x800:
        return "TOP+%03x" % (a & 0x7ff)
    return "%s+%03x" % (b, a & 0xfff)

def ue_len(v): return 2 * (v + 1).bit_length() - 1
def se_len(v):
    v -= (v & 0x80000000) << 1                    # signed
    return ue_len(2 * v - 1 if v > 0 else -2 * v)

def bits(e):
    """Bits the header parser consumed with this read, or None."""
    if e["rw"] != "R": return None
    off = e["addr"] - 0xf0030000
    if e["fn"].startswith("_VDEC_GetFRateFromSPS") and 0 <= off <= 0x80: return off >> 2
    if off == 0x23c: return ue_len(e["val"])
    if off == 0x240: return se_len(e["val"])
    return None

def load(p):
    out = []
    for l in open(p):
        m = re.match(r"(\d+) (\d+) ([RW]) ([0-9a-f]{8}) ([0-9a-f]{8}) (\S+)$", l.strip())
        if m:
            out.append(dict(us=int(m[1]), cpu=int(m[2]), rw=m[3], addr=int(m[4], 16), val=int(m[5], 16), fn=m[6]))
        elif l.startswith("#"):
            print(l.rstrip(), file=sys.stderr)
    return out

def split(es):
    pics, cur = [], []
    for e in es:
        cur.append(e)
        if e["rw"] == "W" and e["addr"] == KICK and e["val"] == 1:
            pics.append(cur); cur = []
    return pics, cur

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace"); ap.add_argument("--pic", type=int); ap.add_argument("--diff", action="store_true")
    a = ap.parse_args()
    pics, rest = split(load(a.trace))
    if a.pic is not None:
        if not 0 <= a.pic < len(pics): sys.exit("only %d decode starts" % len(pics))
        for e in pics[a.pic]:
            b = bits(e)
            print("%9d %s %-9s %08x %s%s" % (e["us"], e["rw"], name(e["addr"]), e["val"], e["fn"],
                                             "" if b is None else "  [%d bits]" % b))
        return
    last, t = {}, None
    for i, p in enumerate(pics):
        # bits since the last barrel shifter init (VLD+0b0 = bitstream address)
        n, bs = 0, None
        for e in p:
            if e["rw"] == "W" and e["addr"] == 0xf00020b0: n, bs = 0, e["val"]
            n += bits(e) or 0
        w = sum(e["rw"] == "W" for e in p)
        print("start %d: t=%d us (+%d), %d writes, %d reads, bitstream %s, %d header bits" % (
            i, p[-1]["us"], p[-1]["us"] - (t if t is not None else p[-1]["us"]), w, len(p) - w,
            "%08x" % bs if bs is not None else "not set", n))
        t = p[-1]["us"]
        now = dict(last)
        for e in p:
            if e["rw"] == "W": now[e["addr"]] = e["val"]
        if a.diff:
            for r in sorted(now):
                if last.get(r) != now[r]:
                    print("    %-9s %s -> %08x" % (name(r), "--------" if r not in last else "%08x" % last[r], now[r]))
        last = now
    print("%d decode starts, %d accesses after the last one" % (len(pics), len(rest)))

main()
