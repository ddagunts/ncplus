#!/usr/bin/env python3
"""
LZHS decompressor for LG NetCast boot images (p1/boot.bin stages: Lmain, U-Boot).

LZHS = LZSS + Huffman, as used by LG. 16-byte header {u32 uncompressed, u32 compressed,
u8 checksum, 7 spare}, then Huffman-coded data that decodes to an LZSS stream.
Algorithm and Huffman tables ported from openlgtv/epk2extract (lzhs.c, tables.h; MIT).

Usage: tools/unlzhs.py <image> <offset> <out>
  e.g. tools/unlzhs.py tv_dump/flashdump/p1_bl.bin 0xa040  lmain.bin   # 2nd-stage loader
       tools/unlzhs.py tv_dump/flashdump/p1_bl.bin 0x80000 uboot.bin   # U-Boot 1.3.4
Prints the header and the decoded size (must equal 'uncompressed') to stderr.
"""
import sys, struct, base64

N = 4096; F = 34; THRESHOLD = 2
_CL = base64.b64decode("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")
_PS = base64.b64decode("AAAAAAIAAAACAAAAAwAAAAYAAAAEAAAADgAAAAUAAAAPAAAABQAAABAAAAAFAAAAEQAAAAUAAAASAAAABQAAABMAAAAFAAAAFAAAAAUAAAAqAAAABgAAACsAAAAGAAAALAAAAAYAAAAtAAAABgAAAC4AAAAGAAAALwAAAAYAAAAwAAAABgAAADEAAAAGAAAAMgAAAAYAAAAzAAAABgAAADQAAAAGAAAANQAAAAYAAAA2AAAABgAAADcAAAAGAAAAOAAAAAYAAAA5AAAABgAAADoAAAAGAAAAOwAAAAYAAAA8AAAABgAAAD0AAAAGAAAAPgAAAAYAAAA/AAAABgAAAA==")
_charlen = [struct.unpack_from("<II", _CL, i*8) for i in range(len(_CL)//8)]  # (code,len)
_pos     = [struct.unpack_from("<II", _PS, i*8) for i in range(len(_PS)//8)]
CMAP = {(l, c): i for i, (c, l) in enumerate(_charlen)}
PMAP = {(l, c): j for j, (c, l) in enumerate(_pos)}

def unhuff(data):
    out = bytearray(); code_buf = [0]*64; cbp = 1; mask = 1
    code = length = 0; pos = 0; n = len(data); cur = 0; bitno = 8
    def getbit():
        nonlocal pos, cur, bitno, code, length
        if bitno > 7:
            bitno = 0
            if pos >= n: return False
            cur = data[pos]; pos += 1
        code = ((code << 1) | ((cur >> (7 - bitno)) & 1)); bitno += 1; length += 1
        return True
    while True:
        if not getbit(): break
        if length < 4: continue
        i = CMAP.get((length, code))
        if i is None: continue
        if i > 255:
            code_buf[cbp] = i - 256; cbp += 1; code = length = 0
            while True:
                if not getbit(): pos = n + 1; break
                if length < 2: continue
                j = PMAP.get((length, code))
                if j is None: continue
                code_buf[cbp] = j >> 1; cbp += 1; break
            if pos > n: break
            jj = j; code = 0; ok = True
            for _ in range(7):
                if not getbit(): ok = False; break
            if not ok: break
            code_buf[cbp] = (code | (jj << 7)) & 0xFF; cbp += 1; code = length = 0
        else:
            code_buf[0] |= mask; code_buf[cbp] = i; cbp += 1; code = length = 0
        mask = (mask << 1) & 0xFF
        if mask == 0:
            out += bytes(code_buf[0:cbp]); code_buf[0] = 0; cbp = 1; mask = 1
    if cbp > 1: out += bytes(code_buf[0:cbp])
    return bytes(out)

def unlzss(data):
    text = bytearray(N); out = bytearray(); r = 0; flags = 0; pos = 0; n = len(data)
    while True:
        flags >>= 1
        if (flags & 256) == 0:
            if pos >= n: break
            flags = data[pos] | 0xff00; pos += 1
        if flags & 1:
            if pos >= n: break
            c = data[pos]; pos += 1
            text[r] = c; out.append(c); r = (r + 1) & (N - 1)
        else:
            if pos + 3 > n: break
            j, i, m = data[pos], data[pos+1], data[pos+2]; pos += 3
            i = (i << 8) | m
            for _ in range(j + THRESHOLD + 1):
                mm = text[(r - i) & (N - 1)]; text[r] = mm; out.append(mm); r = (r + 1) & (N - 1)
    return bytes(out)

def main():
    src, off, outp = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    d = open(src, "rb").read()
    usize, csize, chk = struct.unpack_from("<IIB", d, off)
    sys.stderr.write(f"header: uncompressed={usize:#x} compressed={csize:#x} checksum={chk:#x}\n")
    r = unlzss(unhuff(d[off+16:off+16+csize]))
    open(outp, "wb").write(r)
    sys.stderr.write(f"wrote {outp}: {len(r):#x} bytes ({'OK' if len(r)==usize else 'MISMATCH vs %#x'%usize})\n")

if __name__ == "__main__":
    main()
