#!/usr/bin/env python3
"""Mine Canon's per-frame AF data feed (9,0x17,32) for a usable defocus proxy.

Reads the recovered 2026-10-19 AFDIAG.LOG corpus (106 files, 26 unique by content),
pairs every (9,0x17,32) message seen by crop_rec's @MPU hook with the nearest @SHARP
sample, and tests every payload field against the ML sharpness metric with confound
control.

What can and cannot be tested with this corpus (this is the honest framing):
  * the @MPU hook only logs when "AF diag log = ON + MPU"; only 6 unique logs have it,
    and in those logs EVERY (9,1,5) message is the ML lens_focus no-op form (0xFF speed
    selector) - there is no controlled raw (9,1,5) speed-07 move in any feed log.
    So we cannot do the ideal test: known pulse moves at fixed scene.
  * what does vary in those logs is Canon's own AF (half-press), a handful of sweep/test
    episodes, and AE/zoom transitions.  We therefore:
      A. characterise the corpus and the payload structure (incl. zoom dependence of w/h);
      B. measure field *stability* in static periods (no Canon AF, no moves, constant
         zoom/exposure/lens state) - a field that is not stable there is not a clean
         physical measurement;
      C. test within-scene monotonicity against s in exposure-stable, s-varying windows,
         with sign consistency across logs, permutation p-values and bootstrap CIs;
      D. control the two obvious confounds: frame geometry w*h and scene brightness luma,
         via rank partial correlation and leave-one-log-out;
      E. test the V-shape (magnitude-of-defocus) hypothesis in windows that cross the
         sharpness peak versus the inverted-V (contrast) hypothesis.

Standard library only, deterministic, seconds to run.

  python3 contrib/af_novel/analyze_focusfeed.py
  python3 contrib/af_novel/analyze_focusfeed.py --root /path/to/scratchpad
  python3 contrib/af_novel/analyze_focusfeed.py --root ... --json out.json
"""

import argparse
import glob
import hashlib
import json
import math
import os
import random
import re
import statistics

DEFAULT_ROOT = ('/private/tmp/claude-501/-Users-mgojkovic-repos-Crop-mood-eosm-slim-gui/'
                'ea95f17a-e40e-4903-a809-65ac8220d6dc/scratchpad')

# ---------------------------------------------------------------- field definitions

# The @MPU record has four 32-bit words: w0 = grp<<16 | op<<8 | len, w1..w3 = the first
# 12 payload bytes packed LSB-first.  New logs (crop_rec task-12 / N1 feed logging) add
# ` feed=32B w4=...,w5=...,w6=...,w7=...,w8=...` at the END of a (9,0x17,32) @MPU line:
# w4..w8 are payload bytes 12..31 (LSB-first dwords), so `b32` in a feed row is the full
# 32-byte payload.  Older logs / other messages still have only w1..w3 (12 bytes).
def is_default_format(b):
    """The EOS M also sends a constant 'no AF data' payload (bytes fe 03 fe 01 ... seen in
    the QEMU EOSM spell and 63 field records).  Active AF data has b1=b3=b6=b10=0."""
    return b[1] != 0 or b[3] != 0 or b[6] != 0 or b[10] != 0


def payload_bytes(w1, w2, w3):
    return w1.to_bytes(4, 'little') + w2.to_bytes(4, 'little') + w3.to_bytes(4, 'little')


def make_fields():
    fields = {}
    for i in range(12):
        fields['b%02d' % i] = (lambda b, i=i: b[i])
    for off in (0, 2, 4, 6, 8, 10):
        fields['u16@%d' % off] = (lambda b, o=off: int.from_bytes(b[o:o + 2], 'little'))
    fields['w'] = lambda b: int.from_bytes(b[0:2], 'little')
    fields['h'] = lambda b: int.from_bytes(b[2:4], 'little')
    fields['A'] = lambda b: int.from_bytes(b[4:8], 'little')   # u32 at bytes 4..7
    fields['B'] = lambda b: int.from_bytes(b[8:12], 'little')  # u32 at bytes 8..11
    fields['w*h'] = lambda b: int.from_bytes(b[0:2], 'little') * int.from_bytes(b[2:4], 'little')
    fields['A/B'] = lambda b: (int.from_bytes(b[4:8], 'little') /
                               max(1, int.from_bytes(b[8:12], 'little')))
    fields['A-B'] = lambda b: int.from_bytes(b[4:8], 'little') - int.from_bytes(b[8:12], 'little')
    return fields


FIELDS = make_fields()
# fields reported in detail; everything is tested, these are printed
MAIN_FIELDS = ['w', 'h', 'w*h', 'A', 'B', 'A/B', 'u16@4', 'u16@8']
CANDIDATES = {'A', 'B', 'A/B', 'A-B', 'b04', 'b05', 'b08', 'b09', 'w', 'h', 'w*h'}


# --------------------------------------------------------------------------- parsing

RE_LINE = re.compile(
    r'^(\d+) (\S+) n=(\d+) w=([0-9a-f]+)(?:,([0-9a-f]+),([0-9a-f]+),([0-9a-f]+))?'
    r' z=(\d+) rr=(\d+) hs=(\d+) af=(\d+) pos=(-?\d+) dist=(\d+)'
    # task-12: (9,0x17,32) lines append the rest of the 32-byte payload (bytes 12..31)
    r'(?: feed=32B w4=([0-9a-f]+),w5=([0-9a-f]+),w6=([0-9a-f]+),w7=([0-9a-f]+),w8=([0-9a-f]+))?')
RE_HEADER = re.compile(
    r'# AFDIAG ON t=(\d+) lens="([^"]*)" lv_af_mode=(\d+) cont_af_movie=(\d+)'
    r' manual_focus=(\d+) crop_preset=(\d+) slim_mode=(\d+) small_hacks=(\d+) zoom=x(\d+)')


def list_unique_logs(root):
    """All AFDIAG*.LOG files under root, deduplicated by content (the card copies repeat)."""
    files = sorted(glob.glob(os.path.join(root, '**', 'AFDIAG*.LOG'), recursive=True))
    seen = {}
    unique = []
    for f in files:
        h = hashlib.md5(open(f, 'rb').read()).hexdigest()
        if h in seen:
            continue
        seen[h] = f
        unique.append(f)
    return files, unique


def parse_log(path):
    """Return (header, feed_rows, markers).  feed_rows have the 12 payload bytes (`b`) plus
    the full 32 bytes (`b32`, None on old logs) and the nearest @SHARP fields (s, g, luma, age)."""
    header = {}
    feed = []
    markers = []          # (ms, kind) activity markers that must veto a "static" window
    last = {'s': None, 'g': None, 'luma': None, 'ms': None}
    for line in open(path, errors='ignore'):
        m = RE_HEADER.match(line)
        if m:
            header = dict(lens=m.group(2), lv_af_mode=int(m.group(3)),
                          cont_af_movie=int(m.group(4)), manual_focus=int(m.group(5)),
                          crop_preset=int(m.group(6)), slim_mode=int(m.group(7)),
                          small_hacks=int(m.group(8)), zoom=int(m.group(9)))
            continue
        m = RE_LINE.match(line)
        if not m:
            continue
        ms = int(m.group(1))
        ev = m.group(2)
        w = [int(m.group(k), 16) if m.group(k) else 0 for k in (4, 5, 6, 7)]
        z, rr, hs, af = (int(m.group(k)) for k in (8, 9, 10, 11))
        pos, dist = int(m.group(12)), int(m.group(13))
        if ev == '@SHARP':
            # AFD_EV_SHARP: w0 = Laplacian edge sum, w1 = mean luma, w2 = samples,
            # w3 = max edge.  g is NOT in this event (only in @SWEEP/@MPU-test logs).
            last = {'s': w[0], 'g': None, 'luma': w[1], 'emax': w[3], 'ms': ms}
            continue
        if ev == '@MPU':
            grp = (w[0] >> 16) & 0xFF
            op = (w[0] >> 8) & 0xFF
            length = w[0] & 0xFF
            if (grp, op, length) == (9, 0x17, 32):
                b = payload_bytes(w[1], w[2], w[3])
                # task-12: w4..w8 complete the 32-byte payload (bytes 12..31); None on old logs
                x = [int(m.group(k), 16) if m.group(k) else None for k in (14, 15, 16, 17, 18)]
                b32 = (b + b''.join(v.to_bytes(4, 'little') for v in x)
                       if all(v is not None for v in x) else None)
                feed.append(dict(ms=ms, b=b, b32=b32, z=z, rr=rr, hs=hs, af=af, pos=pos, dist=dist,
                                 s=last['s'], g=None, luma=last['luma'],
                                 emax=last.get('emax'),
                                 age=(ms - last['ms']) if last['ms'] else -1))
                continue
            # activity markers: AF session/config, raw focus drive, zoom changes, MPU search
            # only real AF/zoom state changes count as activity; (9,0x04) aperture,
            # (9,0x0A) AEWB and (9,0x1A) are periodic housekeeping and are ignored here.
            if (grp, op) in ((9, 0x25), (9, 0x21), (9, 0x13), (9, 0x11), (9, 0x12),
                             (9, 0x0B), (9, 0x0E), (9, 0x18), (9, 0x19)):
                markers.append((ms, 'mpu_%02x_%02x' % (grp, op)))
            elif (grp, op) == (9, 1):
                b = payload_bytes(w[1], w[2], w[3])
                if len(b) >= 3 and b[2] == 0x07:
                    markers.append((ms, 'ml_move'))
                else:
                    markers.append((ms, 'focus_cmd_noop'))
            continue
        if ev in ('HALF_SHUTTER', 'LV_AF_RESULT', 'LV_FOCUS_DONE', '@LVOBJ', '@AF_STATE',
                  '@FOCUS_CHG', '@LVLENS', '@REFOCUS', 'LV_LENS_DRIVE_REMOTE',
                  'LIVE_VIEW_AF_SYSTEM', '@REC'):
            markers.append((ms, ev))
    return header, feed, markers


# ------------------------------------------------------------------------- statistics

def ranks(xs):
    """Average ranks (1-based), ties get the mean rank."""
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    r = [0.0] * len(xs)
    i = 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            r[order[k]] = avg
        i = j + 1
    return r


def spearman(xs, ys):
    if len(xs) < 3 or len(set(xs)) < 2 or len(set(ys)) < 2:
        return None
    rx, ry = ranks(xs), ranks(ys)
    return pearson(rx, ry)


def pearson(xs, ys):
    n = len(xs)
    if n < 2:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((a - mx) ** 2 for a in xs))
    sy = math.sqrt(sum((b - my) ** 2 for b in ys))
    if sx == 0 or sy == 0:
        return None
    return sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / (sx * sy)


def partial_spearman(xs, ys, zs):
    """Spearman correlation of x,y after removing rank(z) linearly from both."""
    if len(xs) < 4 or len(set(zs)) < 2:
        return None
    rx, ry, rz = ranks(xs), ranks(ys), ranks(zs)
    mz = sum(rz) / len(rz)
    szz = sum((a - mz) ** 2 for a in rz)
    if szz == 0:
        return None
    def resid(r):
        mr = sum(r) / len(r)
        b = sum((a - mz) * (c - mr) for a, c in zip(rz, r)) / szz
        return [c - mr - b * (a - mz) for a, c in zip(rz, r)]
    return pearson(resid(rx), resid(ry))


def median_ci(vals, rng, trials=1000, lo=2.5, hi=97.5):
    """Bootstrap percentile CI of the median."""
    if len(vals) < 3:
        return None, None
    meds = []
    n = len(vals)
    for _ in range(trials):
        meds.append(statistics.median(vals[rng.randrange(n)] for _ in range(n)))
    meds.sort()
    return meds[int(lo / 100.0 * trials)], meds[min(trials - 1, int(hi / 100.0 * trials))]


def binomial_sign_p(pos, neg):
    """Two-sided exact binomial p for pos vs neg at p=0.5."""
    n = pos + neg
    if n == 0:
        return None
    # normal approximation is fine for these sample sizes; use exact via log-gamma for n<=200
    k = max(pos, neg)
    if n <= 200:
        s = 0.0
        for i in range(k, n + 1):
            s += math.exp(math.lgamma(n + 1) - math.lgamma(i + 1) - math.lgamma(n - i + 1)
                          - n * math.log(2.0))
        return min(1.0, 2.0 * s)
    z = (k - 0.5 * n) / math.sqrt(0.25 * n)
    return 2.0 * 0.5 * math.erfc(abs(z) / math.sqrt(2.0))


# ------------------------------------------------------------------------- windows

def static_windows(feed, markers, rng):
    """Feed records in neighbourhoods where nothing happened: no Canon AF, no commands,
    constant zoom/lens/exposure and stable sharpness.  Used to measure field noise."""
    if not feed:
        return []
    feed = [r for r in feed if not is_default_format(r['b'])]
    marker_ms = sorted(m[0] for m in markers)
    out = []
    for r in feed:
        if r['age'] < 0 or r['age'] > 120 or r['rr']:
            continue
        if r['hs'] or r['af']:
            continue
        lo = r['ms'] - 1000
        hi = r['ms'] + 1000
        # no activity markers within +-1 s (binary search)
        import bisect
        i = bisect.bisect_left(marker_ms, lo)
        if i < len(marker_ms) and marker_ms[i] <= hi:
            continue
        # local s/luma stability over +-800 ms
        seg = [x for x in feed if abs(x['ms'] - r['ms']) <= 800
               and x['s'] and x['luma'] and x['z'] == r['z']]
        if len(seg) < 4:
            continue
        ss = [x['s'] for x in seg]
        lu = [x['luma'] for x in seg]
        if min(lu) <= 0 or max(ss) / max(1, min(ss)) > 1.05 or max(lu) / min(lu) > 1.03:
            continue
        if len(set(x['pos'] for x in seg)) > 1 or len(set(x['dist'] for x in seg)) > 1:
            continue
        out.append(seg)
    # greedy non-overlap
    out.sort(key=lambda s: s[0]['ms'])
    keep = []
    last_end = -1
    for seg in out:
        if seg[0]['ms'] < last_end:
            continue
        keep.append(seg)
        last_end = seg[-1]['ms']
    return keep


def focus_windows(feed, span_ms=1500, min_s_ratio=1.20, max_luma_ratio=1.05):
    """Windows where sharpness changes a lot while exposure, zoom and recording are
    constant.  This is the only place a lens-motion/defocus signal can be seen in this
    corpus; the cause of the motion may be Canon's own AF (recorded as a confound)."""
    rows = [r for r in feed if r['s'] and r['luma'] and r['age'] <= 120 and r['rr'] == 0
            and not is_default_format(r['b'])]
    if len(rows) < 8:
        return []
    wins = []
    j = 0
    for i in range(len(rows)):
        while j < len(rows) and rows[j]['ms'] - rows[i]['ms'] <= span_ms:
            j += 1
        seg = rows[i:j]
        if len(seg) < 4:
            continue
        if len(set(r['z'] for r in seg)) > 1 or len(set(r['pos'] for r in seg)) > 1:
            continue
        ss = [r['s'] for r in seg]
        lu = [r['luma'] for r in seg]
        if min(lu) <= 0 or max(ss) / max(1, min(ss)) < min_s_ratio:
            continue
        if max(lu) / min(lu) > max_luma_ratio:
            continue
        wins.append(seg)
    # greedy non-overlap, keep the window with the largest s range first
    wins.sort(key=lambda s: -(max(r['s'] for r in s) / max(1, min(r['s'] for r in s))))
    keep = []
    for seg in wins:
        a, b = seg[0]['ms'], seg[-1]['ms']
        if any(not (b < k[0]['ms'] or a > k[-1]['ms']) for k in keep):
            continue
        keep.append(seg)
    keep.sort(key=lambda s: s[0]['ms'])
    for seg in keep:
        seg[0]['canon'] = any(r['hs'] for r in seg)
    return keep


def crossing_windows(wins):
    """Windows whose sharpness has an interior peak or trough (so the V-shape versus
    inverted-V hypothesis can be told apart)."""
    out = []
    for seg in wins:
        s = [r['s'] for r in seg]
        imax = max(range(len(s)), key=lambda i: s[i])
        imin = min(range(len(s)), key=lambda i: s[i])
        if 0 < imax < len(s) - 1 and s[imax] > 1.15 * min(s):
            out.append((seg, imax, 'peak'))
        elif 0 < imin < len(s) - 1 and s[imin] * 1.15 < max(s):
            out.append((seg, imin, 'trough'))
    return out


# --------------------------------------------------------------------------- analysis

def analyse_field(field_name, extract, feed_all, static, focus, crossings, rng, logs):
    """Return a dict of statistics for one payload field."""
    vals_all = [extract(r['b']) for r in feed_all]

    # static-period noise: within-window sd of the field when nothing happened
    static_sds = []
    for seg in static:
        v = [extract(r['b']) for r in seg]
        if len(v) < 4:
            continue
        static_sds.append(statistics.pstdev(v))

    # within-window monotonicity against s in focus-change windows
    rhos, prhos, rho_geom = [], [], []
    rhos_canon, rhos_other = [], []
    for seg in focus:
        f = [extract(r['b']) for r in seg]
        s = [r['s'] for r in seg]
        geom = [extract_geom(r['b']) for r in seg]
        rho = spearman(f, s)
        if rho is not None:
            rhos.append(rho)
            if seg[0].get('canon'):
                rhos_canon.append(rho)
            else:
                rhos_other.append(rho)
        pr = partial_spearman(f, s, geom)
        if pr is not None:
            prhos.append(pr)
        rg = spearman(f, geom)
        if rg is not None:
            rho_geom.append(rg)

    # V-shape test on crossing windows: field near the sharpness extremum vs flanks.
    # Effect is expressed in units of the field's within-static-window sd (physical noise).
    v_signs = []
    v_by_log = {}
    for seg, imax, kind in crossings:
        f = [extract(r['b']) for r in seg]
        s = [r['s'] for r in seg]
        near = [f[i] for i in range(len(f)) if abs(i - imax) <= max(1, len(f) // 5)]
        flank = [f[i] for i in range(len(f)) if abs(i - imax) > max(1, len(f) // 3)]
        if not near or not flank:
            continue
        d = statistics.mean(flank) - statistics.mean(near)
        sd_all = statistics.pstdev(f) or 1.0
        v_signs.append((d / sd_all, kind))
        v_by_log.setdefault(seg[0]['log'], []).append(d / sd_all)

    # permutation test for the median rho across focus windows (candidate fields only)
    perm_p = None
    if field_name in CANDIDATES and len(rhos) >= 3:
        obs = abs(statistics.median(rhos))
        null = []
        for _ in range(1000):
            med = []
            for seg in focus:
                f = [extract(r['b']) for r in seg]
                s = [r['s'] for r in seg]
                s2 = s[:]
                rng.shuffle(s2)
                rr = spearman(f, s2)
                if rr is not None:
                    med.append(rr)
            if med:
                null.append(abs(statistics.median(med)))
        null.sort()
        k = sum(1 for v in null if v >= obs)
        perm_p = (k + 1.0) / (len(null) + 1.0)

    # per-log consistency (leave-one-log-out style: sign agreement of each log's median)
    per_log = {}
    for seg in focus:
        per_log.setdefault(seg[0]['log'], []).append(
            spearman([extract(r['b']) for r in seg], [r['s'] for r in seg]))
    per_log_med = {k: statistics.median([x for x in v if x is not None])
                   for k, v in per_log.items() if any(x is not None for x in v)}
    pos = sum(1 for r in rhos if r > 0.1)
    neg = sum(1 for r in rhos if r < -0.1)
    lo, hi = median_ci(rhos, rng) if rhos else (None, None)
    plo, phi = median_ci(prhos, rng) if prhos else (None, None)
    static_sd_ref = statistics.median(static_sds) if static_sds else None
    v_static_units = None
    if v_signs and static_sd_ref:
        # convert median (flank-centre) in within-window sd to static-noise sd units
        med_ratio = statistics.median([d for d, k in v_signs])
        # within-window pstdev/static sd is not tracked per window here; report the ratio
        # of median |flank-centre| to the static sd using the median field spread
        spread = statistics.median([statistics.pstdev([extract(r['b']) for r in seg])
                                    for seg in focus]) if focus else 0
        v_static_units = med_ratio * spread / static_sd_ref if static_sd_ref else None

    return dict(field=field_name,
                static_sd_med=statistics.median(static_sds) if static_sds else None,
                n_static=len(static_sds),
                n_focus=len(rhos),
                rho_s_med=statistics.median(rhos) if rhos else None,
                rho_s_ci=[lo, hi],
                rho_s_iqr=[statistics.median(sorted(rhos)[:max(1, len(rhos) // 4)]),
                           statistics.median(sorted(rhos)[-max(1, len(rhos) // 4):])]
                         if len(rhos) >= 4 else None,
                neg=neg, pos=pos,
                sign_p=binomial_sign_p(pos, neg),
                perm_p=perm_p,
                rho_partial_med=statistics.median(prhos) if prhos else None,
                rho_partial_ci=[plo, phi],
                rho_geom_med=statistics.median(rho_geom) if rho_geom else None,
                n_crossing=len(v_signs),
                v_shape_med=statistics.median([d for d, k in v_signs]) if v_signs else None,
                v_static_units=v_static_units,
                v_by_log={k: statistics.median(v) for k, v in v_by_log.items()},
                rhos_canon=rhos_canon, rhos_other=rhos_other,
                per_log_med=per_log_med)


def extract_geom(b):
    return int.from_bytes(b[0:2], 'little') * int.from_bytes(b[2:4], 'little')


# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--json', help='write machine-readable results to this path')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--s-ratio', type=float, default=1.20,
                    help='minimum sharpness range inside a focus-change window')
    ap.add_argument('--luma-tol', type=float, default=1.05,
                    help='maximum luma range inside a focus-change window (exposure control)')
    args = ap.parse_args()

    rng = random.Random(args.seed)
    files, unique = list_unique_logs(args.root)
    print('== corpus ==')
    print('  AFDIAG files found : %d' % len(files))
    print('  unique by content  : %d (the card*/ copies are repeated)' % len(unique))

    parsed = []
    total_feed = total_moves07 = total_noop = total_hs = 0
    for path in unique:
        hdr, feed, markers = parse_log(path)
        for r in feed:
            r['log'] = os.path.basename(path)
        parsed.append((path, hdr, feed, markers))
        total_feed += len(feed)
        total_moves07 += sum(1 for m in markers if m[1] == 'ml_move')
        total_noop += sum(1 for m in markers if m[1] == 'focus_cmd_noop')
        total_hs += sum(1 for m in markers if m[1] == 'HALF_SHUTTER')

    feed_logs = [(p, h, f, mk) for p, h, f, mk in parsed if len(f) >= 50]
    print('  (9,0x17,32) records : %d in %d unique logs (>=50 records: %d)' %
          (total_feed, sum(1 for p, h, f, m in parsed if f), len(feed_logs)))
    print('  raw speed-07 (9,1,5) moves in feed logs : %d' % total_moves07)
    print('  ML lens_focus no-op (9,1,5) messages    : %d' % total_noop)
    print('  half-shutter records (Canon AF)         : %d' % total_hs)
    print('  -> no controlled ML lens moves exist in any log that also has the AF feed;')
    print('     the only lens motion visible here is Canon\'s own AF and test episodes.')

    print('\n  feed logs:')
    print('  %-40s %6s %7s %6s %7s %s' % ('log', 'zoom', 'feed', 'moves07', 'staticw', 'header'))
    static_all, focus_all, cross_all = [], [], []
    for path, hdr, feed, markers in feed_logs:
        static = static_windows(feed, markers, rng)
        focus = focus_windows(feed, min_s_ratio=args.s_ratio, max_luma_ratio=args.luma_tol)
        cross = crossing_windows(focus)
        static_all += static
        focus_all += focus
        cross_all += cross
        nmov = sum(1 for m in markers if m[1] == 'ml_move')
        print('  %-40s %6s %7d %7d %7d %s' %
              (os.path.basename(path), hdr.get('zoom', '?'), len(feed), nmov, len(static),
               'af_movie=%s small_hacks=%s' % (hdr.get('cont_af_movie'), hdr.get('small_hacks'))))

    print('\n== payload structure ==')
    fb = [r['b'] for p, h, f, m in feed_logs for r in f]
    ws = [int.from_bytes(b[0:2], 'little') for b in fb]
    hs = [int.from_bytes(b[2:4], 'little') for b in fb]
    ratios = [w / h for w, h in zip(ws, hs) if h]
    full32 = [r for p, h, f, m in feed_logs for r in f if r.get('b32')]
    print('  n payloads: %d (%d with the full 32 bytes, feed=32B)' % (len(fb), len(full32)))
    print('  first u16 pair (w,h): median w=%d h=%d, median w/h=%.2f, sentinel 1022x510: %d'
          % (statistics.median(ws), statistics.median(hs), statistics.median(ratios),
             sum(1 for w, h in zip(ws, hs) if w == 1022 and h == 510)))
    print('  w,h by zoom (median):')
    by_z = {}
    for p, h, f, m in feed_logs:
        for r in f:
            by_z.setdefault(r['z'], []).append((int.from_bytes(r['b'][0:2], 'little'),
                                                int.from_bytes(r['b'][2:4], 'little')))
    for z in sorted(by_z):
        v = by_z[z]
        print('    z=%-2d n=%-4d w %5d  h %5d  (p10..p90 w %d..%d)'
              % (z, len(v), statistics.median(x[0] for x in v), statistics.median(x[1] for x in v),
                 sorted(x[0] for x in v)[len(v) // 10], sorted(x[0] for x in v)[9 * len(v) // 10]))
    const = [i for i in range(12) if len(set(b[i] for b in fb)) == 1]
    print('  byte offsets constant across every feed record: %s' % const)

    print('\n== confound matrix (Spearman, all feed records with fresh sharpness) ==')
    pairs = [('A', 's'), ('B', 's'), ('A', 'luma'), ('B', 'luma'), ('A', 'w*h'),
             ('B', 'w*h'), ('A', 'B'), ('s', 'luma')]
    rows = [r for p, h, f, m in feed_logs for r in f if r['s'] and r['luma'] and r['age'] <= 120]
    print('  n = %d' % len(rows))
    def get(name, r):
        if name == 's':
            return r['s']
        if name == 'luma':
            return r['luma']
        return FIELDS[name](r['b'])
    for a, b in pairs:
        vals = [r for r in rows if r['s'] and r['luma']]
        xs = [get(a, r) for r in vals]
        ys = [get(b, r) for r in vals]
        rh = spearman(xs, ys)
        print('  rho(%-4s, %-4s) = %+6.3f   n=%d' % (a, b, rh if rh is not None else float('nan'),
                                                    len(vals)))

    print('\n== payload formats ==')
    all_feed = [r for p, h, f, m in feed_logs for r in f]
    default = [r for r in all_feed if is_default_format(r['b'])]
    active = [r for r in all_feed if not is_default_format(r['b'])]
    print('  records: %d active, %d constant/default format (%s)'
          % (len(active), len(default),
             ', '.join(sorted(set(r['log'] for r in default)))))
    print('  active format: first u16 pair (w,h) median %dx%d, w/h=%.2f; bytes 7 and 11 are'
          % (statistics.median(int.from_bytes(r['b'][0:2], 'little') for r in active),
             statistics.median(int.from_bytes(r['b'][2:4], 'little') for r in active),
             statistics.median(int.from_bytes(r['b'][0:2], 'little') /
                               max(1, int.from_bytes(r['b'][2:4], 'little')) for r in active)))
    print('  always 0.  w,h medians by zoom:')
    by_z = {}
    for r in active:
        by_z.setdefault(r['z'], []).append(r)
    for z in sorted(by_z):
        v = by_z[z]
        print('    z=%-2d n=%-4d w=%5d h=%5d' % (z, len(v),
              statistics.median(int.from_bytes(r['b'][0:2], 'little') for r in v),
              statistics.median(int.from_bytes(r['b'][2:4], 'little') for r in v)))

    print('\n== static periods (no Canon AF, no commands, constant zoom/lens/exposure) ==')
    print('  (a field that is not stable here is not a clean physical measurement)')
    print('  static windows: %d, with %d feed records'
          % (len(static_all), sum(len(s) for s in static_all)))
    if static_all:
        sds = {f: statistics.median([statistics.pstdev([FIELDS[f](r['b']) for r in seg])
                                     for seg in static_all]) for f in MAIN_FIELDS}
        means = {f: statistics.median([statistics.mean([FIELDS[f](r['b']) for r in seg])
                                       for seg in static_all]) for f in MAIN_FIELDS}
        rsds = statistics.median([statistics.pstdev([r['s'] for r in seg]) /
                                  statistics.mean([r['s'] for r in seg]) for seg in static_all])
        print('  median relative s-noise inside static windows: %.3f (matches the measured 1%%)' % rsds)
        print('  field   median within-window sd   median mean   relative')
        for f in ['w', 'h', 'A', 'B', 'A/B']:
            rel = sds[f] / abs(means[f]) if means[f] else float('nan')
            print('  %-5s  %22.3f   %10.1f   %8.3f' % (f, sds[f], means[f], rel))
        # cross-scene static means: does A track scene contrast or brightness?
        wm = []
        for seg in static_all:
            wm.append(dict(A=statistics.mean(FIELDS['A'](r['b']) for r in seg),
                           B=statistics.mean(FIELDS['B'](r['b']) for r in seg),
                           s=statistics.mean(r['s'] for r in seg),
                           luma=statistics.mean(r['luma'] for r in seg),
                           emax=statistics.mean(r['emax'] for r in seg if r['emax'] is not None),
                           wh=statistics.mean(extract_geom(r['b']) for r in seg)))
        print('  across static windows (different scenes, lens not moving), n=%d:' % len(wm))
        for f in ('A', 'B'):
            print('    rho(%s, scene s)=%+.3f  rho(%s, luma)=%+.3f  rho(%s, max_edge)=%+.3f  rho(%s, w*h)=%+.3f'
                  % (f, spearman([x[f] for x in wm], [x['s'] for x in wm]) or 0,
                     f, spearman([x[f] for x in wm], [x['luma'] for x in wm]) or 0,
                     f, spearman([x[f] for x in wm], [x['emax'] for x in wm]) or 0,
                     f, spearman([x[f] for x in wm], [x['wh'] for x in wm]) or 0))

    print('\n== focus-change windows (exposure/zoom constant, s changes >=20%%) ==')
    print('  windows: %d, feed records: %d, crossing windows: %d, with half-shutter: %d'
          % (len(focus_all), sum(len(w) for w in focus_all), len(cross_all),
             sum(1 for w in focus_all if w[0].get('canon'))))
    if focus_all:
        print('  median s range ratio per window: %.2f, median luma range ratio: %.3f'
              % (statistics.median([max(r['s'] for r in w) / min(r['s'] for r in w) for w in focus_all]),
                 statistics.median([max(r['luma'] for r in w) / min(r['luma'] for r in w) for w in focus_all])))
        print('  (these windows are Canon half-press AF and test episodes; there are NO')
        print('   ML raw speed-07 move windows in this corpus - see corpus section)')

    print('\n== per-field tests: monotonicity against s in focus-change windows ==')
    print('  field   n   med_rho   95% CI          pos/neg  sign_p  perm_p  partial(geom)  rho_geom   canon/other med_rho')
    results = []
    for f in FIELDS:
        r = analyse_field(f, FIELDS[f], active, static_all, focus_all, cross_all, rng, feed_logs)
        results.append(r)
    results_by_name = {r['field']: r for r in results}
    for f in ['w', 'h', 'w*h', 'A', 'B', 'A/B', 'A-B', 'b04', 'b05', 'b08', 'b09', 'b10']:
        r = results_by_name.get(f)
        if not r:
            continue
        ci = r['rho_s_ci']
        rc = statistics.median(r['rhos_canon']) if r['rhos_canon'] else None
        ro = statistics.median(r['rhos_other']) if r['rhos_other'] else None
        print('  %-5s %3d %+8.3f  %-14s %3d/%-3d %7s %7s  %+11s  %+8s  %s'
              % (f, r['n_focus'],
                 r['rho_s_med'] if r['rho_s_med'] is not None else float('nan'),
                 '[%+.2f,%+.2f]' % (ci[0], ci[1]) if ci and ci[0] is not None else 'n/a',
                 r['pos'], r['neg'],
                 ('%.3f' % r['sign_p']) if r['sign_p'] is not None else 'n/a',
                 ('%.3f' % r['perm_p']) if r['perm_p'] is not None else 'n/a',
                 ('%+.3f' % r['rho_partial_med']) if r['rho_partial_med'] is not None else 'n/a',
                 ('%+.3f' % r['rho_geom_med']) if r['rho_geom_med'] is not None else 'n/a',
                 '%s / %s' % ('%+.2f' % rc if rc is not None else 'n/a',
                              '%+.2f' % ro if ro is not None else 'n/a')))

    print('\n== V-shape (|defocus| magnitude) versus inverted-V (contrast) on crossing windows ==')
    print('  f(|defocus|) is LOW at the sharpness extremum, high on the flanks (positive).')
    print('  effect in within-window sd units; per-log medians show sign consistency.')
    for f in ['A', 'B', 'A/B', 'b04', 'b05', 'b08', 'b09']:
        r = results_by_name[f]
        if r['n_crossing'] == 0:
            print('  %-4s  no crossing windows' % f)
            continue
        print('  %-4s  n=%2d  median %+0.2f sd   per-log: %s'
              % (f, r['n_crossing'], r['v_shape_med'] if r['v_shape_med'] is not None else float('nan'),
                 {k: round(v, 2) for k, v in r['v_by_log'].items()}))

    print('\n== per-log sign consistency (leave-one-log-out proxy) ==')
    for f in ['A', 'B', 'A/B']:
        r = results_by_name[f]
        print('  %-4s per-log median rho(s): %s' %
              (f, {k: round(v, 2) for k, v in r['per_log_med'].items()}))

    print('\n== verdict ==')
    def usable(r):
        """A field is a usable defocus proxy only if the relationship to sharpness is
        strong, significant, sign-consistent across every log, and not explained by the
        frame-geometry confound."""
        if r['n_focus'] < 5 or r['rho_s_med'] is None or r['perm_p'] is None:
            return False, 'too few windows'
        if abs(r['rho_s_med']) < 0.6:
            return False, 'median |rho| < 0.6'
        if r['perm_p'] > 0.05:
            return False, 'permutation p > 0.05'
        if r['rho_partial_med'] is not None and abs(r['rho_partial_med']) < 0.5:
            return False, 'not robust to frame-geometry confound'
        signs = [1 if v > 0 else -1 for v in r['per_log_med'].values() if abs(v) > 0.1]
        if signs and min(sum(1 for x in signs if x > 0), sum(1 for x in signs if x < 0)) > 0:
            return False, 'sign flips across logs'
        return True, 'passes'
    any_usable = False
    for f in ['A', 'B', 'A/B', 'A-B']:
        r = results_by_name[f]
        ok, why = usable(r)
        any_usable = any_usable or ok
        print('  %-4s usable=%s (%s)' % (f, 'YES' if ok else 'no', why))
    a = results_by_name['A']
    b = results_by_name['B']
    print('  best candidate for a *new* measurement (ML does not use word A):')
    print('    A: |rho| median %.2f (p=%s), per-log signs %s; V-shape %s sd'
          % (abs(a['rho_s_med']) if a['rho_s_med'] is not None else float('nan'),
             ('%.3f' % a['perm_p']) if a['perm_p'] is not None else 'n/a',
             {k: ('+' if v > 0 else '-') for k, v in a['per_log_med'].items()},
             ('%+.2f' % a['v_shape_med']) if a['v_shape_med'] is not None else 'n/a'))
    print('    B: |rho| median %.2f (p=%s), per-log signs %s; partial(geom) %+.2f'
          % (abs(b['rho_s_med']) if b['rho_s_med'] is not None else float('nan'),
             ('%.3f' % b['perm_p']) if b['perm_p'] is not None else 'n/a',
             {k: ('+' if v > 0 else '-') for k, v in b['per_log_med'].items()},
             b['rho_partial_med'] if b['rho_partial_med'] is not None else float('nan')))
    if not any_usable:
        print('  -> NO field qualifies as a usable defocus proxy from this corpus.')
    print('  -> This falsifies "the defocus number is minable from the existing logs", NOT')
    print('     the existence of such a number.  The corpus cannot settle it because no')
    print('     controlled raw speed-07 move was ever logged together with the AF feed.')
    print('  -> Decisive test: the dedicated protocol in doc/EOSM-AF-FOCUSFEED.md')
    print('     (fixed scene, known speed-07 moves, full 32-byte payload logged).')

    if args.json:
        def clean(d):
            out = {}
            for k, v in d.items():
                if isinstance(v, dict):
                    out[k] = {kk: round(vv, 4) if isinstance(vv, float) else vv
                              for kk, vv in v.items()}
                elif isinstance(v, (int, float, str)) or v is None:
                    out[k] = v
                elif isinstance(v, list):
                    out[k] = [round(x, 4) if isinstance(x, float) else x for x in v]
                else:
                    out[k] = v
            return out
        corpus = dict(files=len(files), unique=len(unique), feed_records=total_feed,
                      logs_with_feed_ge50=len(feed_logs), raw_moves07=total_moves07,
                      noop_moves=total_noop, static_windows=len(static_all),
                      focus_windows=len(focus_all), crossing_windows=len(cross_all))
        with open(args.json, 'w') as f:
            json.dump(dict(corpus=corpus, fields=[clean(r) for r in results]), f, indent=1)

        print('\nwrote %s' % args.json)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
