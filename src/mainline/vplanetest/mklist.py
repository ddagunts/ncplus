#!/usr/bin/env python3
"""mklist.py <now> <play1> <play2> <outdir> [luma_phys]
Build the replay lists for the first video-plane experiment from tools/vplanerecon.sh snapshots:
<now> is the state to change (a Debian snapshot), <play1>/<play2> two snapshots of the working
state: on stock while LG's player showed a 720p clip, or (better: no LG interrupt handlers
stirring) on a Debian booted mid-playback (play2 also holds b2r-x3.txt).

  1-b2r.list   the CKGEN words LG's player changed (B2R clock divider +0x22c, enable +0x228 bit 0
               last, ...) and the HD channel's configuration from the playback snapshot, with
               luma/chroma pointed at luma_phys (default 0x2c000000, RAM hole) and the channel
               left disabled
  2-path.list  every word of 0xf0022000..0xf0025000, 0xf0027000, 0xf005e000 and the MJC 0xf0036000
               (needed: without LG's MJC values the panel stays black) that is the same in both
               playback snapshots and different now (words that changed between the two playback
               snapshots are status or counters and are left alone)
  3-on.list    B2R enable as LG's _VDP_HalSetEnable does it (+0x40: 0x11, then |4)
  4-trig.list  the main path's import triggers (LG's VRM, vDrvFire*ImportTrigger): PDS 0xf0023164
               bit 4, regen 0xf00231b0 bit 25, deinterlacer 0xf0024010 bit 27, front scaler
               0xf0025228 bit 1 pulse. Without them the size registers keep the old input's values
               and the panel stays black.
Apply 1-b2r, 3-on, 2-path, 4-trig with `vplane w <list> > <list>.undo`; undo in reverse order."""
import os, sys

def load(f):
    w = {}
    for l in open(f):
        a, rest = l.split(':', 1)
        a = int(a, 16)
        b = rest.split()[:16]
        for i in range(len(b) // 4):
            w.setdefault(a + 4 * i, int(''.join(reversed(b[4 * i:4 * i + 4])), 16))
    return w

now, p1, p2, out = sys.argv[1:5]
luma = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x2c000000
W, H = 1280, 720
chroma = luma + W * ((H + 31) // 32 * 32)
os.makedirs(out, exist_ok=True)

b2r = load(os.path.join(p2, 'b2r-x3.txt'))
ck0, ck1, ck2 = (load(os.path.join(d, '0xf000d000.txt')) for d in (now, p1, p2))
B = 0xf0026000
with open(os.path.join(out, '1-b2r.list'), 'w') as f:
    # clock words that differ from LG's playback state (B2R clock divider +0x22c and the ones
    # LG's player changes with it); the B2R clock enable (+0x228 bit 0) last
    f.write('# CKGEN as LG left it while playing 720p30\n')
    cks = [r for r in sorted(ck1) if ck1[r] == ck2.get(r) and ck1[r] != ck0.get(r)]
    for r in sorted(cks, key=lambda r: r == 0xf000d228):
        f.write('%08x %08x  # was %08x\n' % (r, ck1[r], ck0[r]))
    f.write('# HD channel off while it is set up\n%08x %08x\n' % (B + 0x40, 0))
    f.write('%08x %08x  # luma\n%08x %08x  # chroma\n' % (B, luma, B + 4, chroma))
    f.write('%08x %08x  # right view luma (_VDP_HalSetRViewAddr; LG keeps it = left in 2D)\n'
            '%08x %08x  # right view chroma\n' % (B + 0x50, luma, B + 0x54, chroma))
    for o in range(0x08, 0x60, 4):
        if o in (0x30, 0x3c, 0x40, 0x50, 0x54):   # trigger, status, enable, right view (above)
            continue
        f.write('%08x %08x\n' % (B + o, b2r[B + o]))
    f.write('%08x %08x  # shared control\n' % (B + 0x300, b2r[B + 0x300]))

N, P1, P2 = {}, {}, {}
pages = [pg for pg in (0xf0022000, 0xf0023000, 0xf0024000, 0xf0025000, 0xf0027000, 0xf005e000, 0xf0036000)
         if all(os.path.exists(os.path.join(d, '0x%08x.txt' % pg)) for d in (now, p1, p2))]
n = 0
with open(os.path.join(out, '2-path.list'), 'w') as f:
    for pg in pages:
        a, b, c = (load(os.path.join(d, '0x%08x.txt' % pg)) for d in (now, p1, p2))
        f.write('# page %08x\n' % pg)
        for r in sorted(b):
            if b[r] == c.get(r) and b[r] != a.get(r):
                f.write('%08x %08x  # was %08x\n' % (r, b[r], a[r]))
                n += 1
with open(os.path.join(out, '3-on.list'), 'w') as f:
    f.write('%08x %08x\n%08x %08x\n%08x %08x  # update\n' % (B + 0x40, 0x11, B + 0x40, 0x15, B + 0x30, 1))
with open(os.path.join(out, '4-trig.list'), 'w') as f:
    f.write('f0023164 00000010 00000010  # PDS import\n'
            'f00231b0 02000000 02000000  # regen import\n'
            'f0024010 08000000 08000000  # deinterlacer set-res trigger\n'
            'f0025228 00000002 00000002  # front scaler import off\n'
            'f0025228 00000000 00000002  # ... and on\n')
print('luma %08x chroma %08x, %d path words' % (luma, chroma, n))
