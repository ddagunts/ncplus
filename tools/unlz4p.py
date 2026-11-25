#!/usr/bin/env python3
"""Decode an MTK "LZ4P" container (LG NetCast kernel partitions p21/p22).

Layout (little-endian):
  0x00  'LZ4P'
  0x04  u32 uncompressed size
  0x08  u32 total container size (header + size table + blocks)
  0x0c  u32 block size (uncompressed, 0x10000)
  0x10  u32 number of blocks
  0x14  (zero padding up to 0x20)
  0x20  u32[nblocks] compressed size of each block
  then the raw LZ4 blocks back to back.
A block whose compressed size equals the block size is stored uncompressed.

usage: unlz4p.py in.bin out.bin
"""
import struct, sys


def lz4_block(src, max_out):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(out):
            raise ValueError("bad match offset %d at out %d" % (off, len(out)))
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    if len(out) > max_out:
        raise ValueError("block overflow")
    return bytes(out)


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"LZ4P":
        sys.exit("not an LZ4P container")
    usize, csize, bsize, nblk = struct.unpack_from("<4I", data, 4)
    sizes = struct.unpack_from("<%dI" % nblk, data, 0x20)
    pos = 0x20 + 4 * nblk
    out = bytearray()
    for k, cs in enumerate(sizes):
        blk = data[pos:pos + cs]; pos += cs
        want = min(bsize, usize - len(out))
        if cs == want:
            out += blk
        else:
            d = lz4_block(blk, bsize)
            if len(d) != want:
                raise SystemExit("block %d: got %d bytes, want %d" % (k, len(d), want))
            out += d
    if len(out) != usize or pos != csize:
        raise SystemExit("size mismatch: out %d/%d, consumed %d/%d" %
                         (len(out), usize, pos, csize))
    open(sys.argv[2], "wb").write(out)
    print("%s: %d blocks, %d -> %d bytes" % (sys.argv[1], nblk, csize, usize))


if __name__ == "__main__":
    main()
