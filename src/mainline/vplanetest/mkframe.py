#!/usr/bin/env python3
"""mkframe.py <in.nv12> <w> <h> <out.tiled>
Lay an NV12 picture out the way the MT5398 decoder writes it (and B2R reads it): 64x32 luma
tiles, then 64x16 chroma tiles (CbCr interleaved), both in raster tile order, the height padded
to 32 lines (the inverse of untile() in mt5398-vdec.c). Make the input with e.g.
  ffmpeg -ss 5 -i clip.mp4 -frames:v 1 -pix_fmt nv12 -f rawvideo in.nv12"""
import sys

src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
d = open(src, 'rb').read()
stride = (w + 63) // 64 * 64
ph = (h + 31) // 32 * 32
assert len(d) >= w * h * 3 // 2, 'short input'

def plane(base, pw, rows, th, fill):
    out = bytearray()
    for ty in range(0, rows, th):
        for tx in range(0, stride, 64):
            for y in range(th):
                yy = ty + y
                if yy < rows and tx < pw:
                    o = base + yy * pw + tx
                    row = d[o:o + min(64, pw - tx)]
                else:
                    row = b''
                out += row + bytes([fill]) * (64 - len(row))
    return out

luma = plane(0, w, h, 32, 16)
luma += bytes([16]) * (stride * ph - len(luma))
chroma = plane(w * h, w, h // 2, 16, 128)
chroma += bytes([128]) * (stride * ph // 2 - len(chroma))
open(dst, 'wb').write(luma + chroma)
print('%s: luma %#x bytes, chroma at +%#x' % (dst, len(luma), len(luma)))
