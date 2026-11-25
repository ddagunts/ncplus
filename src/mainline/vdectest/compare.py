#!/usr/bin/env python3
"""compare.py out.bin ref.yuv [w=1280 h=720]   out.bin = 1280x736 decoder output, ref = ffmpeg yuv420p.
Try the layouts the hardware might use and report how many luma/chroma samples match."""
import sys
W, H, PH = 1280, 720, 736
d = open(sys.argv[1], "rb").read(); r = open(sys.argv[2], "rb").read()
L = W * PH; luma, chroma = d[:L], d[L:]
ry, ru, rv = r[:W*H], r[W*H:W*H*5//4], r[W*H*5//4:W*H*3//2]
def detile(src, w, h, tw, th):
    out = bytearray(w * h); i = 0
    for ty in range(0, h, th):
        for tx in range(0, w, tw):
            for y in range(th):
                if ty + y < h: out[(ty+y)*w+tx:(ty+y)*w+tx+tw] = src[i:i+tw]
                i += tw
    return bytes(out)
def match(a, b): return 100.0 * sum(x == y for x, y in zip(a, b)) / len(b)
best = None
for name, (tw, th) in {"64x32 (MT5398)": (64, 32), "MM21 16x32": (16, 32), "16x16": (16, 16), "32x16": (32, 16), "linear": (W, 1)}.items():
    y = detile(luma, W, PH, tw, th)[:W*H] if name != "linear" else luma[:W*H]
    m = match(y, ry); print("luma  %-10s %6.2f %%" % (name, m))
    if best is None or m > best[0]: best = (m, name, tw, th)
tw, th = best[2], max(1, best[3] // 2)
c = detile(chroma, W, PH//2, tw, th) if best[1] != "linear" else chroma
u = c[0::2][:W*H//4]; v = c[1::2][:W*H//4]
print("chroma UV %6.2f %6.2f | VU %6.2f %6.2f (layout %s)" % (match(u, ru), match(v, rv), match(v, ru), match(u, rv), best[1]))
open("out_luma.gray", "wb").write(detile(luma, W, PH, best[2], best[3])[:W*H])
