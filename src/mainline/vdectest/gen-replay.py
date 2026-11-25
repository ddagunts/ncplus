#!/usr/bin/env python3
"""gen-replay.py <ncvdec trace> <decode start>... > replay.txt
Register sequence for one or more pictures (in decode order), each from the last hardware reset
before its slice data to its decode start. One op per line:
  P n                 start of picture n (index in the trace)
  W reg val           plain write
  W reg @TAG sh add   address register: (address of TAG >> sh) + add, from vdectest's buffer.
                      TAG: FIFO FIFOEND WORK WPTR, ES:<offset> (byte in the elementary stream),
                      FB<k> (frame k's luma), FBC<k> (its chroma), MV<k> (motion vector buffer k)
  R reg expected      bitstream read (consumes bits); must match the value stock saw
  S reg expected      stock's sync reads: vdectest reads until it sees the last expected value
  B                   barrel shifter: poll until ready
Writing 1 to AVLD+238 starts the decode; vdectest then waits for it to finish.
Frame and MV buffers are numbered in the order stock first wrote into them (MC+560, AMV+20c).
"""
import os, sys

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "../../../tools/vdectrace.py")).read().replace("\nmain()\n", "\n")
vt = {"__name__": "vt"}; exec(compile(src, "vdectrace.py", "exec"), vt)
pics, _ = vt["split"](vt["load"](sys.argv[1]))
SYNC = 4                      # stock's sync reads after each barrel init (seen in the traces)
fbs, mvs = [], []             # stock addresses, in slot order

def slot(lst, a):
    if a not in lst: lst.append(a)
    return lst.index(a)

for n in map(int, sys.argv[2:]):
    pic = pics[n]
    last = max(i for i, e in enumerate(pic) if e["rw"] == "W" and e["addr"] == 0xf00020b0)
    start = max(i for i in range(last) if pic[i]["rw"] == "W" and pic[i]["addr"] == 0xf0002108 and pic[i]["val"] == 0x101)
    fifo = None
    print("P %d" % n)
    sync = None
    for e in pic[start:]:
        a, v = e["addr"], e["val"]
        if e["rw"] == "R":
            if e["fn"].startswith("_VDEC_GetFRateFromSPS") or a in (0xf003023c, 0xf0030240):
                if sync is not None and a == 0xf0030020 and sync < SYNC:
                    print("S %08x %08x" % (a, v)); sync += 1
                else:
                    print("R %08x %08x" % (a, v))
            continue
        tag = None
        if a == 0xf00020b4: fifo = v << 6; tag = ("FIFO", 6, 0)
        elif a == 0xf00020b8: tag = ("FIFOEND", 6, 0)
        elif a == 0xf0002110: tag = ("WPTR", 0, 0)
        elif a == 0xf0002828: tag = ("WORK", 0, 0)
        elif a == 0xf00020b0: tag = ("ES:%x" % (v - fifo - 4), 0, 0)
        elif a == 0xf000a560: tag = ("FB%d" % slot(fbs, v), 0, 0)
        elif a == 0xf000a224: tag = ("FB%d" % slot(fbs, v << 9), 9, 0)
        elif a == 0xf000a228: tag = ("FBC%d" % slot(fbs, (v << 8) - 0xe6000), 8, 0)
        elif 0xf000a3dc <= a < 0xf000a55c and v: tag = ("FB%d" % slot(fbs, v), 0, 0)      # reference lists
        elif a == 0xf003120c: tag = ("MV%d" % slot(mvs, v << 4), 4, 0)
        elif 0xf0031180 <= a < 0xf0031200 and v: tag = ("MV%d" % slot(mvs, (v & ~0xf) << 4), 4, v & 0xf)  # colocated MVs
        if tag: print("W %08x @%s %d %d" % (a, *tag))
        else: print("W %08x %08x" % (a, v))
        if a == 0xf000208c and v == 0x00100000: print("B"); sync = 0
print("# %d frame buffers, %d motion vector buffers" % (len(fbs), len(mvs)))
