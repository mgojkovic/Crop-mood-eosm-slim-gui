#!/usr/bin/env python3
"""What is really inside (9,0x17,32) - Canon's per-frame AF data feed on the EOS M?

The crop_rec "@MPU" hook already logs the first 12 bytes of every ICU->MPU message.
This tool extracts the (9,0x17,32) records from an AFDIAG.LOG (AF diag log ON + MPU) or
from the CSV sample in samples/, prints what can be learned from the existing field data,
and prints the exact protocol for the decisive follow-up session.

FINDINGS THIS TOOL REPRODUCES (see doc/EOSM-AF-NOVEL.md idea N1):
  * (9,0x17,32) really is a per-frame feed: it appears at the hook's 250 ms rate limit for
    the whole session, ~637 unique payloads in 647 records (Refocus test log).  It is NOT
    the ML property 0x80050026 arriving through the property layer: that handler never
    fires on the EOS M (no @LV_FOCUS_DATA line exists in any field log).
  * the first four payload bytes behave like two little-endian u16: an AF-frame width and
    height.  The idle/no-AF value is 1022 x 510 (the same sentinel is in the QEMU EOSM
    spell for PROP_LV_FOCUS_DATA), active values are e.g. 88 x 44 or 26 x 12.
  * bytes 4..7 ("A") and 8..11 ("B") are two more scalar fields.  Across the Refocus log,
    A rises as the Laplacian sharpness falls (correlation with 1/s = +0.64) while B does
    not; B is roughly positively related to sharpness in other logs.  That is consistent
    with A being a Canon-computed defocus/focus-difference magnitude and B a contrast
    evaluation - which would be exactly the one-step measurement we want - but it is
    confounded by Canon's own zoom/AF activity, so it is a hypothesis, not a result.

If A is a signed or magnitude defocus field, one half-press or one two-position probe can
replace the coarse search.  That is the single cheapest decisive experiment on this camera.

Usage:
  python3 contrib/af_novel/focusfeed_probe.py
  python3 contrib/af_novel/focusfeed_probe.py --log /path/to/AFDIAG.LOG
  python3 contrib/af_novel/focusfeed_probe.py --log ... --csv out.csv
"""

import argparse
import math
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE_CSV = os.path.join(HERE, 'samples', 'focusfeed_AFDIAG_card.csv')

RE_MPU = re.compile(r'^(\d+) @MPU n=(\d+) w=([0-9a-f]+),([0-9a-f]+),([0-9a-f]+),([0-9a-f]+)')
RE_SHARP = re.compile(r'^(\d+) @SHARP n=\d+ w=([0-9a-f]+),')
RE_HDR = re.compile(r'^# AFDIAG ON t=(\d+) lens="([^"]*)"')


def read_csv(path):
    rows = []
    for line in open(path):
        if line.startswith('#'):
            continue
        f = line.strip().split(',')
        if len(f) < 6:
            continue
        try:
            ms, fw, fh, A, B = (int(f[0]), int(f[1]), int(f[2]), int(f[3]), int(f[4]))
            sharp = int(f[5]) if f[5] else 0
            age = int(f[6]) if f[6] else -1
            pos = int(f[7]) if f[7] else 0
            dist = int(f[8]) if f[8] else 0
            zoom = int(f[9]) if f[9] else 0
        except ValueError:
            continue
        rows.append(dict(ms=ms, fw=fw, fh=fh, A=A, B=B, s=sharp, age=age,
                         pos=pos, dist=dist, zoom=zoom, idle=(fw == 1022 and fh == 510)))
    return rows


def read_afdiag(path):
    """Parse an AFDIAG.LOG into the same rows (first 12 bytes of (9,0x17,32))."""
    rows = []
    header = {}
    last_s = 0
    last_s_ms = -1
    for line in open(path, errors='ignore'):
        m = RE_HDR.match(line)
        if m:
            header = {'t': int(m.group(1)), 'lens': m.group(2)}
            continue
        m = RE_SHARP.match(line)
        if m:
            last_s = int(m.group(2), 16)
            last_s_ms = int(m.group(1))
            continue
        m = RE_MPU.match(line)
        if not m:
            continue
        ms = int(m.group(1))
        w0 = int(m.group(3), 16)
        if ((w0 >> 8) & 0xffff) != 0x0917 or (w0 & 0xff) != 32:
            continue
        b = b''.join(int(m.group(k), 16).to_bytes(4, 'little') for k in (4, 5, 6))
        fw = int.from_bytes(b[0:2], 'little')
        fh = int.from_bytes(b[2:4], 'little')
        A = int.from_bytes(b[4:8], 'little')
        B = int.from_bytes(b[8:12], 'little')
        rows.append(dict(ms=ms, fw=fw, fh=fh, A=A, B=B, s=last_s,
                         age=ms - last_s_ms if last_s_ms >= 0 else -1,
                         pos=0, dist=0, zoom=0, idle=(fw == 1022 and fh == 510)))
    return rows, header


def corr(x, y):
    n = len(x)
    if n < 3:
        return 0.0
    mx, my = sum(x) / n, sum(y) / n
    sx = math.sqrt(sum((a - mx) ** 2 for a in x))
    sy = math.sqrt(sum((b - my) ** 2 for b in y))
    if sx * sy == 0:
        return 0.0
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy)


def analyse(rows, label):
    print('\n== %s: %d (9,0x17,32) records ==' % (label, len(rows)))
    if not rows:
        return
    idle = [r for r in rows if r['idle']]
    act = [r for r in rows if not r['idle']]
    print('  idle (w=1022,h=510): %d   active: %d' % (len(idle), len(act)))
    pairs = {}
    for r in act:
        pairs[(r['fw'], r['fh'])] = pairs.get((r['fw'], r['fh']), 0) + 1
    print('  most common active (frame_w, frame_h): %s'
          % sorted(pairs.items(), key=lambda kv: -kv[1])[:6])
    if not act:
        return
    with_s = [r for r in act if r['s'] and r['age'] < 200]
    print('  active records with a fresh sharpness sample (<200 ms): %d' % len(with_s))
    if len(with_s) > 10:
        s = [r['s'] for r in with_s]
        print('  sharpness range in those records: %d .. %d' % (min(s), max(s)))
        print('  corr(A, 1/s) = %+.2f    corr(B, 1/s) = %+.2f    corr(A, B) = %+.2f'
              % (corr([r['A'] for r in with_s], [1.0 / r['s'] for r in with_s]),
                 corr([r['B'] for r in with_s], [1.0 / r['s'] for r in with_s]),
                 corr([r['A'] for r in with_s], [r['B'] for r in with_s])))
        print('  binned by sharpness (A and B medians):')
        bins = {}
        for r in with_s:
            bins.setdefault(min(9, r['s'] // 10000), []).append(r)
        for k in sorted(bins):
            v = bins[k]
            print('    s ~ %2dk  n=%3d  A med %5d   B med %5d   frame %dx%d'
                  % (k * 10, len(v), statistics.median(r['A'] for r in v),
                     statistics.median(r['B'] for r in v),
                     statistics.median(r['fw'] for r in v),
                     statistics.median(r['fh'] for r in v)))
        print('  interpretation: if A is monotone in 1/s (defocus magnitude) and B tracks s')
        print('  (contrast), a *known-position* probe could turn A/B into a one-step jump.')
        print('  This log alone cannot separate that from Canon changing its own AF frame/zoom.')


PROTOCOL = """
== decisive field session: decode the Canon AF data feed (no rebuild needed for step 1) ==

Logging (current firmware is enough for step 1):
  Settings -> AF diag log: ON + MPU     (this enables the @MPU hook and @SHARP lines)
  Settings -> AF drive monitor: Keep open
  x5, crop preset active, EF-M 18-55 STM, IS off, MF drive not touched.

Procedure (one session, ~30 min):
  1. five static subjects at measured distances d = 0.30, 0.50, 1.0, 2.0, 5.0 m,
     each a high-contrast chart / textured box, camera on a tripod, same framing size.
  2. at each distance: (a) 10 s still, no AF; (b) one Canon half-press AF (x5);
     (c) move the lens by -200, -100, -40, +40, +100, +200 pulses at speed 07 with
     AF drive (MPU) and let it settle; log every step.  The sweep record is not needed:
     the @MPU hook already sees (9,0x17,32) four times a second.
  3. repeat at x10 for the same five distances.

Expected signal (this decides the project direction):
  * if A (payload bytes 4..7) is monotone in |defocus| and changes sign or grows smoothly
    with the commanded pulse moves at fixed subject distance -> Canon computes a defocus
    magnitude; a one-step jump becomes a decoding problem.  Verify it does NOT simply
    follow the AF frame w/h (a geometry field would move with zoom, not with the lens).
  * if B (bytes 8..11) tracks Laplacian s -> it is a contrast evaluation value; useful as
    an exposure-invariant peak detector, not as a jump.
  * if neither field moves with the lens at fixed scene -> the feed is frame geometry /
    status only; kill N1 and spend the effort on the two-position multi-band estimator.

Then (only if A is promising), one small logging change: widen the @MPU capture to all 32
bytes for (9,0x17) and log every frame, not 4 Hz; repeat step 2.  That patch belongs to
the crop_rec workstream, not to contrib/.

Kill criteria: A/B unchanged across the five distances and the six pulse offsets for two
different scenes, or A and B both explained by the AF frame geometry.  In that case stop.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', help='AFDIAG.LOG to parse (default: use the CSV sample)')
    ap.add_argument('--csv', help='write the parsed rows to this CSV')
    args = ap.parse_args()

    if args.log:
        rows, header = read_afdiag(args.log)
        analyse(rows, os.path.basename(args.log))
        if header:
            print('  header: lens=%s t=%d' % (header.get('lens'), header.get('t')))
    elif os.path.exists(SAMPLE_CSV):
        rows = read_csv(SAMPLE_CSV)
        analyse(rows, 'samples/focusfeed_AFDIAG_card.csv (2026-10-19 Refocus test)')
    else:
        print('no log and no sample CSV; run with --log AFDIAG.LOG')
        return 1

    if args.csv and args.log:
        with open(args.csv, 'w') as f:
            f.write('ms,frame_w,frame_h,A,B,sharp,age_ms,idle\n')
            for r in rows:
                f.write('%d,%d,%d,%d,%d,%d,%d,%d\n' % (r['ms'], r['fw'], r['fh'], r['A'],
                                                       r['B'], r['s'], r['age'], r['idle']))
        print('wrote %s' % args.csv)

    print(PROTOCOL)
    return 0


if __name__ == '__main__':
    sys.exit(main())
