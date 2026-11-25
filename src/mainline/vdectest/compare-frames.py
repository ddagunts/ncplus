#!/usr/bin/env python3
"""compare-frames.py out.bin ref.yuv   out.bin = vdectest's frame buffers (1280x736, 64x32/64x16 tiles)
one after the other; ref.yuv = ffmpeg yuv420p 1280x720 frames. Reports which ref frame each buffer
equals."""
import sys
W, H, PH = 1280, 720, 736
FS, RS = W * PH * 3 // 2, W * H * 3 // 2
d = open(sys.argv[1], "rb").read(); r = open(sys.argv[2], "rb").read()
def detile(src, w, h, tw, th):
    out = bytearray(w * h); i = 0
    for ty in range(0, h, th):
        for tx in range(0, w, tw):
            for y in range(th):
                out[(ty + y) * w + tx:(ty + y) * w + tx + tw] = src[i:i + tw]; i += tw
    return bytes(out)
refs = [r[i:i + RS] for i in range(0, len(r) - RS + 1, RS)]
for k in range(len(d) // FS):
    f = d[k * FS:(k + 1) * FS]
    y = detile(f[:W * PH], W, PH, 64, 32)[:W * H]
    c = detile(f[W * PH:], W, PH // 2, 64, 16); yuv = y + c[0::2][:W * H // 4] + c[1::2][:W * H // 4]
    open("out_frame%d.yuv" % k, "wb").write(yuv)
    for j, rf in enumerate(refs):
        same = sum(a == b for a, b in zip(yuv, rf))
        print("buffer %d vs ref frame %d: %s (%.3f %% of bytes equal)" % (k, j, "IDENTICAL" if yuv == rf else "differs", 100.0 * same / RS))
