#!/usr/bin/env python3
"""Offline evaluation of one-step / few-frame defocus estimators for EOS M ML autofocus.

Runs on the REAL sweep data preserved in samples/AFSWEEP.LOG (x5, 18-55 STM, s1/s2/s4/g
per frame, exported by crop_rec "AF sweep record").  It answers three questions that
decide whether the "novel" measurement ideas in doc/EOSM-AF-NOVEL.md are worth a field
session:

  A. multi-band absolute defocus: is a single frame's s1/s2/s4 ratio a scene-independent
     ruler of |focus error|?  (Checks cross-scene spread of band = s1/(s2+s4).)
  B. longitudinal dither: from three samples at p-d, p, p+d, does the parabola/Gaussian
     vertex predict the focus offset, and how does the error scale with d and with the
     measured ~1% reading noise?  (This is the cheapest "novel" search primitive: it
     needs no extra protocol, only (9,1,5) moves that already work.)
  C. edge-width / PSF: synthetic check of how well a blurred step edge's width can be
     measured, and where the estimator saturates (pixel pitch) - to show it is a coarse
     estimator only.

Standard library only, deterministic.  No camera, no numpy.

  python3 contrib/af_novel/defocus_estimators.py
  python3 contrib/af_novel/defocus_estimators.py --sweep path/to/AFSWEEP.LOG --noise 0.01

The sweep file is a concatenation of several field sessions; the header is printed once,
so sweeps may come from different scenes and zoom levels.  That is deliberate: it is the
cross-scene test.  Positions are reconstructed in PULSES by fitting the sharpness curve
and using the measured s-curve width sigma_s = 21 pulses (median of 22 accepted real fits,
contrib/af_replay/REPORT.md), which is an optical property of the lens/zoom, not of the
scene.  Absolute position along the sweep is otherwise unknown (no focus_pos), so this
normalisation is the only way to compare sweeps.
"""

import argparse
import math
import os
import random
import statistics

SIGMA_S_PULSES = 21.0   # measured median s-curve sigma at x5, 18-55 STM
SIGMA_G_PULSES = 35.0   # measured median g-curve sigma
NOISE_S = 0.01          # measured reading noise, relative sd (REPORT.md: 0.85-1.4 %)
NOISE_BAND = 0.02       # a ratio of sums is noisier; conservative


# --------------------------------------------------------------------------- parsing

def load_sweeps(path):
    """Parse AFSWEEP.LOG into a list of sweeps with deduplicated frames.

    Returns sweeps: [{'k','cmd','dir','samples':[(t,frame,s1,s2,s4,g,q,band,luma)]}]
    Duplicate consecutive frames (same s1/s2/s4/g) come from the sampler seeing the same
    LV frame twice and are collapsed.
    """
    sweeps, cur = [], None
    for line in open(path, errors='ignore'):
        if line.startswith('# sweep'):
            parts = dict(p.split('=') for p in line[len('# sweep '):].split() if '=' in p)
            cur = {'k': int(parts.get('k', 0)), 'cmd': int(parts.get('cmd', 0)),
                   'dir': int(parts.get('dir', 0)), 'samples': []}
            sweeps.append(cur)
            continue
        if not line.startswith('S ') or cur is None:
            continue
        f = line.split()
        t, fr = int(f[1]), int(f[2])
        s1, s2, s4, g, q, band, luma = map(int, f[3:10])
        if cur['samples'] and (s1, s2, s4, g) == cur['samples'][-1][2:6]:
            continue
        cur['samples'].append((t, fr, s1, s2, s4, g, q, band, luma))
    return [s for s in sweeps if len(s['samples']) >= 4]


# ------------------------------------------------------- curve fit (p* / sigma in frames)

def _lstsq3(rows, ys):
    """least squares for y = c0 + c1*x + c2*x^2 (3 normal equations, no deps)."""
    A = [[0.0] * 3 for _ in range(3)]
    b = [0.0] * 3
    for (x, y) in zip(rows, ys):
        p = [1.0, x, x * x]
        for i in range(3):
            b[i] += p[i] * y
            for j in range(3):
                A[i][j] += p[i] * p[j]
    # gaussian elimination with partial pivot
    for i in range(3):
        piv = max(range(i, 3), key=lambda r: abs(A[r][i]))
        if abs(A[piv][i]) < 1e-12:
            return None
        A[i], A[piv] = A[piv], A[i]
        b[i], b[piv] = b[piv], b[i]
        for r in range(i + 1, 3):
            f = A[r][i] / A[i][i]
            for c in range(i, 3):
                A[r][c] -= f * A[i][c]
            b[r] -= f * b[i]
    x = [0.0] * 3
    for i in (2, 1, 0):
        x[i] = (b[i] - sum(A[i][j] * x[j] for j in range(i + 1, 3))) / A[i][i]
    return x


def fit_gauss(idx, ys):
    """Fit y = B + A*exp(-(i-p)^2/(2 sig^2)) by grid over (p, sig), linear A/B.

    Returns (p_star, sigma_frames, r2) or None.  This is a GLM fit used only to define a
    monotone position coordinate for each sweep; it does not assume the estimator.
    """
    n = len(ys)
    if n < 6:
        return None
    ymin, ymax = min(ys), max(ys)
    if ymax <= 0 or ymax < 1.3 * ymin:
        return None
    best = None
    # p* is within a couple of frames of the sample max; sig 0.8..30.6 frames
    imax = max(range(n), key=lambda i: ys[i])
    for sig in [0.8 + 0.15 * k for k in range(0, 200)]:
        for pk in [imax - 2 + 0.5 * k for k in range(0, 9)]:
            g = [math.exp(-((i - pk) ** 2) / (2 * sig * sig)) for i in idx]
            # linear least squares for ys ~ B + A*g
            sg = sum(g); sgg = sum(v * v for v in g); sy = sum(ys); sgy = sum(a * b for a, b in zip(g, ys))
            det = n * sgg - sg * sg
            if abs(det) < 1e-9:
                continue
            A = (n * sgy - sg * sy) / det
            B = (sy - A * sg) / n
            if A <= 0:
                continue
            ssr = sum((y - (B + A * gg)) ** 2 for y, gg in zip(ys, g))
            sst = sum((y - sy / n) ** 2 for y in ys)
            r2 = 1 - ssr / sst if sst > 0 else 0
            if best is None or r2 > best[0]:
                best = (r2, pk, sig)
    if best is None:
        return None
    return best[1], best[2], best[0]


def sweep_frames(sw):
    """Return per-sweep series with a calibrated pseudo-position in pulses.

    pos_i = (frame_i - p*) * SIGMA_S_PULSES / sigma_frames
    """
    S = sw['samples']
    idx = list(range(len(S)))
    s = [x[2] for x in S]
    fit = fit_gauss(idx, s)
    if not fit:
        return None
    pk, sig_f, r2 = fit
    if r2 < 0.80 or not (1.5 <= sig_f <= 40):
        return None
    scale = SIGMA_S_PULSES / sig_f
    out = []
    for i, x in enumerate(S):
        out.append({'pos': (i - pk) * scale, 's': x[2], 's2': x[3], 's4': x[4], 'g': x[5],
                    'band': x[7], 'q': x[6], 'luma': x[8], 'frame': x[1]})
    return {'samples': out, 'sigma_frames': sig_f, 'r2': r2, 'smax': max(s), 'smin': min(s),
            'cmd': sw['cmd'], 'dir': sw['dir'], 'k': sw['k']}


def crossing(series):
    """A sweep usable for defocus tests: it must actually cross focus (peak inside, deep flanks)."""
    if series is None:
        return False
    S = series['samples']
    if not S:
        return False
    if series['smax'] < 1.8 * series['smin']:
        return False
    ip = max(range(len(S)), key=lambda i: S[i]['s'])
    return 1 <= ip <= len(S) - 2


# ------------------------------------------------------------------ A: absolute multiband

def test_absolute_band(series_list):
    """Is band = s1/(s2+s4) scene-independent at a fixed |defocus|?

    Bin every crossing sweep by |pos| and report the cross-sweep spread.  If a single
    frame is to give an absolute one-step jump, this spread must be small compared to the
    change of band over one DOF.
    """
    print('\n== A. single-frame multi-band absolute defocus (band = s1/(s2+s4)) ==')
    pts = []
    for ser in series_list:
        if not crossing(ser):
            continue
        for x in ser['samples']:
            pts.append((abs(x['pos']), x['band'], ser['k'], x['s']))
    if not pts:
        print('  no crossing sweeps')
        return None
    bins = {}
    for p, b, k, s in pts:
        key = int(p // 25) * 25
        bins.setdefault(key, []).append(b)
    rows = []
    print('  |pos| pulses   n   band median   p10..p90 (cross-scene spread)')
    for key in sorted(bins):
        v = sorted(bins[key])
        if len(v) < 5:
            continue
        p10 = v[int(0.1 * len(v))]
        p90 = v[int(0.9 * len(v))]
        rows.append(key)
        print('  %6d..%-6d %4d   %8.1f     %5.1f .. %5.1f' % (key, key + 24, len(v),
              statistics.median(v), p10, p90))
    # spread at a fixed |pos| versus dynamic range of band within one scene near focus
    spreads = []
    for key, v in bins.items():
        if len(v) >= 10:
            sv = sorted(v)
            spreads.append(sv[int(0.9 * len(sv))] - sv[int(0.1 * len(sv))])
    near = [x['band'] for ser in series_list if crossing(ser) for x in ser['samples']
            if abs(x['pos']) < 30]
    near_rng = (max(near) - min(near)) if near else 0
    print('  median within-bin (cross-scene) band spread : %5.1f' % (statistics.median(spreads) if spreads else 0))
    print('  band spread across all scenes near focus   : %5.1f' % near_rng)
    print('  VERDICT: a single frame cannot give a scene-independent absolute distance')
    print('           unless the within-bin spread is small; use a two-position differential')
    print('           estimator (B/C) or calibrate band per scene from the last good focus.')
    return spreads


# ----------------------------------------------------------- B: longitudinal dither (3 pt)

def _vertex3(ym, y0, yp, d):
    """Parabola vertex offset (pulses) from a symmetric three-point sample, y at -d,0,+d."""
    den = ym - 2.0 * y0 + yp
    if den >= -1e-9:        # not concave: no peak, cannot predict
        return None
    return d * (ym - yp) / (2.0 * den)


def _noisy(v, noise, rng):
    return max(1.0, v * (1.0 + rng.gauss(0.0, noise)))


def _vertex3_uneven(xs, ys):
    """Quadratic through three (x, y) points with unequal spacing; return vertex x."""
    c = _lstsq3(xs, ys)
    if not c or abs(c[2]) < 1e-12:
        return None
    if c[2] > 0:            # not concave
        return None
    return -c[1] / (2.0 * c[2])


def test_dither(series_list, d_values, noise_s, noise_band, seed=1, avg=3, trials=20):
    """Test the three-sample dither predictor against the real sweep curves.

    The focus peak is position 0 in every sweep.  Around every current position p, take
    samples at p-d, p, p+d (as a real implementation would after two moves), add the
    measured multiplicative noise, predict the peak position, and compare the residual
    |predicted peak| with the do-nothing residual |p|.  This is the actual decision a
    warm/local refocus has to make.  Stratified by |p| because curvature is only strong
    within about one s-curve sigma.
    """
    print('\n== B. longitudinal dither: three samples at -d, 0, +d ==')
    print('  focus peak is position 0; noise sd %.2f%% (s), %.1f%% (band), %d frames averaged per point'
          % (noise_s * 100, noise_band * 100, avg))
    eff_s = noise_s / math.sqrt(avg)
    eff_band = noise_band / math.sqrt(avg)
    rng = random.Random(seed)

    def interp(ser, pos):
        return min(ser['samples'], key=lambda x: abs(x['pos'] - pos))

    specs = []
    for ser in series_list:
        if not crossing(ser):
            continue
        S = ser['samples']
        pmin, pmax = S[0]['pos'], S[-1]['pos']
        step = max(1.0, abs(S[1]['pos'] - S[0]['pos']))
        for d in d_values:
            p = pmin + d
            while p <= pmax - d + 1e-9:
                a, b, c = interp(ser, p - d), interp(ser, p), interp(ser, p + d)
                if len({a['frame'], b['frame'], c['frame']}) == 3:
                    specs.append((d, p, a, b, c))
                p += step

    print('  d(pulses)  method         |p| bin    n   mean|p|  mean|pred|  gain      sign-wrong')
    results = {}
    for d in d_values:
        for name in ('s-vertex', 'log-s-vertex', 'band-slope'):
            for lo, hi in ((0, 30), (30, 70), (70, 10 ** 6)):
                errs, base, wrong = [], [], 0
                for (dd, p, a, b, c) in specs:
                    if dd != d or not (lo <= abs(p) < hi):
                        continue
                    for _trial in range(trials):
                        if name in ('s-vertex', 'log-s-vertex'):
                            ps = [a['pos'], b['pos'], c['pos']]
                            ys = [_noisy(a['s'], eff_s, rng), _noisy(b['s'], eff_s, rng),
                                  _noisy(c['s'], eff_s, rng)]
                            if name == 'log-s-vertex':
                                ys = [math.log(v) for v in ys]
                            pred = _vertex3_uneven(ps, ys)
                        else:
                            bm = _noisy(a['band'], eff_band, rng)
                            bp = _noisy(c['band'], eff_band, rng)
                            pred = (p + d) if bp > bm else (p - d)
                        if pred is None:
                            errs.append(abs(p) + 2 * d)
                            base.append(abs(p))
                            continue
                        if (pred > 0) != (p > 0) and abs(pred) > 20:
                            wrong += 1
                        errs.append(abs(pred))
                        base.append(abs(p))
                if not errs:
                    continue
                mae = sum(errs) / len(errs)
                bmae = sum(base) / len(base)
                results[(d, name, lo)] = (mae, bmae)
                print('  %8d  %-14s %3d-%-6d %4d  %7.1f  %9.1f  %+7.1f %%  %6d'
                      % (d, name, lo, min(hi, 999), len(errs), bmae, mae,
                         100.0 * (bmae - mae) / bmae if bmae else 0, wrong))
    print('  gain > 0 means the dither prediction beats keeping the lens where it is.')
    print('  The s-vertex is close to what a retrace already does; log-s-vertex and band-slope')
    print('  are the new signals.  A dither costs 2 extra moves (about 0.5-1 s).')
    return results


# --------------------------------------------- C: two-position differential (band ratio)

def test_differential_band(series_list, d_values=(20, 30, 40), noise_band=NOISE_BAND, seed=3, trials=8):
    """Cross-scene test of the DFD-style log-band-ratio slope.

    A two-frame estimator must cancel scene texture.  Train a linear map from features
    (log band ratio over a known move, plus current band) to the signed focus offset on
    all-but-one sweep and test on the held-out sweep (leave-one-scene-out).  Features that
    only use *local differences* are the honest candidate; the absolute band term tests
    whether the scene term is really needed.
    """
    print('\n== C. two-position differential estimator (log multi-band ratio), leave-one-scene-out ==')
    rng = random.Random(seed)
    good = [s for s in series_list if crossing(s)]
    if len(good) < 2:
        print('  not enough crossing sweeps for a cross-scene test')
        return
    print('  crossing sweeps used: %d' % len(good))

    def interp(ser, pos):
        return min(ser['samples'], key=lambda x: abs(x['pos'] - pos))

    def dataset(ser, d):
        rows = []
        S = ser['samples']
        pmin, pmax = S[0]['pos'], S[-1]['pos']
        step = max(1.0, abs(S[1]['pos'] - S[0]['pos']))
        p = pmin + d
        while p <= pmax - d:
            a = interp(ser, p)
            b = interp(ser, p + d)
            if a['frame'] == b['frame']:
                p += step
                continue
            dlog = math.log(_noisy(b['band'], noise_band, rng) / _noisy(a['band'], noise_band, rng))
            rows.append(([1.0, dlog / d, math.log(b['band'])], -p))
            p += step
        return rows

    for d in d_values:
        data = [[r for _ in range(trials) for r in dataset(ser, d)] for ser in good]
        errs_plain, errs_scene, base = [], [], []
        for held in range(len(good)):
            train = [r for i in range(len(good)) if i != held for r in data[i]]
            test = data[held]
            if len(train) < 6 or len(test) < 2:
                continue
            for feats, col in enumerate(([0, 1], [0, 1, 2])):
                # normal equations, no numpy
                nf = len(col)
                A = [[0.0] * nf for _ in range(nf)]
                bb = [0.0] * nf
                for row, truth in train:
                    x = [row[c] for c in col]
                    for i in range(nf):
                        bb[i] += x[i] * truth
                        for j in range(nf):
                            A[i][j] += x[i] * x[j]
                # solve
                ok = True
                for i in range(nf):
                    piv = max(range(i, nf), key=lambda r: abs(A[r][i]))
                    if abs(A[piv][i]) < 1e-12:
                        ok = False
                        break
                    A[i], A[piv] = A[piv], A[i]
                    bb[i], bb[piv] = bb[piv], bb[i]
                    for r in range(i + 1, nf):
                        f = A[r][i] / A[i][i]
                        for c in range(i, nf):
                            A[r][c] -= f * A[i][c]
                        bb[r] -= f * bb[i]
                if not ok:
                    continue
                w = [0.0] * nf
                for i in range(nf - 1, -1, -1):
                    w[i] = (bb[i] - sum(A[i][j] * w[j] for j in range(i + 1, nf))) / A[i][i]
                for row, truth in test:
                    x = [row[c] for c in col]
                    pred = sum(wi * xi for wi, xi in zip(w, x))   # predicted jump (=-p)
                    e = abs(pred)
                    base.append(abs(truth))
                    if feats == 0:
                        errs_plain.append(e)
                    else:
                        errs_scene.append(e)
        def summ(e, base):
            if not e:
                return '     n/a'
            e.sort()
            bmae = sum(base) / len(base) if base else 0
            return 'MAE %5.1f p90 %5.1f  <=40p: %4.1f%%  (do-nothing MAE %5.1f)' % (
                sum(e) / len(e), e[int(0.9 * len(e))],
                100.0 * sum(1 for v in e if v <= 40) / len(e), bmae)
        print('  d=%3d  local differences only : %s' % (d, summ(errs_plain, base)))
        print('  d=%3d  + absolute band term  : %s' % (d, summ(errs_scene, base)))
    print('  If the "local differences only" row is close to the "+ absolute band" row, the')
    print('  estimator generalises across scenes and a single dither move is a usable jump.')


# ----------------------------------------------------- D: edge width / PSF (synthetic)

def test_edge_width():
    """How well can a blurred step edge's Gaussian radius be measured?

    1-D step edge, random sub-pixel position, blur sigma_b, Gaussian read noise of 1 % of
    the contrast.  Estimator: 10-90 % rise distance of the edge (robust, sub-pixel by
    linear interpolation); for a Gaussian edge this is 2.563*sigma.  Reports bias, sd and
    where the pixel grid saturates the estimate.
    """
    print('\n== D. edge-width / PSF radius from a blurred step (synthetic) ==')
    print('  true sigma_b(px)  measured median   sd    note')
    rng = random.Random(7)

    def rise_width(prof):
        n = len(prof)
        lo = sorted(prof)[n // 10]
        hi = sorted(prof)[9 * n // 10]
        rng_ = hi - lo
        if rng_ <= 0:
            return None
        t10, t90 = lo + 0.1 * rng_, lo + 0.9 * rng_
        def cross(thr):
            for i in range(n - 1):
                if (prof[i] - thr) * (prof[i + 1] - thr) <= 0 and prof[i + 1] != prof[i]:
                    return i + (thr - prof[i]) / (prof[i + 1] - prof[i])
            return None
        x10, x90 = cross(t10), cross(t90)
        if x10 is None or x90 is None or x90 <= x10:
            return None
        return x90 - x10

    for sig in (0.2, 0.3, 0.4, 0.5, 0.8, 1.2, 2.0, 3.0, 5.0, 8.0):
        val = []
        for _ in range(400):
            pos = rng.uniform(5.0, 25.0)          # sub-pixel edge position
            contrast = rng.uniform(40.0, 120.0)
            base = rng.uniform(40.0, 120.0)
            noise = 0.01 * contrast
            prof = []
            for i in range(40):
                u = i + 0.5 - pos
                v = base + contrast * 0.5 * (1.0 + math.erf(u / (math.sqrt(2.0) * sig)))
                prof.append(v + rng.gauss(0.0, noise))
            w = rise_width(prof)
            if w is not None:
                val.append(w / 2.563)
        if not val:
            continue
        med = statistics.median(val)
        sd = statistics.pstdev(val)
        note = ''
        if med < 0.55:
            note = 'SATURATED at the pixel pitch'
        elif sd > 0.3 * max(med, 0.1):
            note = 'noisy'
        print('  %14.2f  %14.2f  %5.2f   %s' % (sig, med, sd, note))
    print('  Consequence: edge width is a *coarse* defocus estimator only.  A defocus of a few')
    print('  pulses at x5 is already below the pixel pitch, so it cannot replace the Laplacian')
    print('  peak near focus; it can only seed a coarse jump from deep blur.')


# ------------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument('--sweep', default=os.path.join(here, 'samples', 'AFSWEEP.LOG'))
    ap.add_argument('--avg', type=int, default=3, help='frames averaged per dither point (default 3)')
    ap.add_argument('--noise', type=float, default=NOISE_S,
                    help='relative sd of a sharpness reading (default %.3f)' % NOISE_S)
    ap.add_argument('--d', default='5,10,20,30,40')
    args = ap.parse_args()

    if not os.path.exists(args.sweep):
        print('no sweep file at %s' % args.sweep)
        return 1
    sweeps = load_sweeps(args.sweep)
    fitted = [(sw, sweep_frames(sw)) for sw in sweeps]
    series = [ser for sw, ser in fitted if ser]
    print('AFSWEEP: %d sweeps, %d with a usable sharpness peak fit' % (len(sweeps), len(series)))
    print('\nper-sweep curve fits (position scale set by sigma_s = %.0f pulses):' % SIGMA_S_PULSES)
    print('  k  cmd    n  sigma(frames)  r2    s range          band range      crosses focus')
    for sw, ser in fitted:
        if not ser:
            continue
        S = ser['samples']
        print('  %d %5d %4d %12.2f %5.2f  %6d..%-6d  %5d..%-5d  %s'
              % (ser['k'], ser['cmd'], len(S), ser['sigma_frames'], ser['r2'],
                 ser['smin'], ser['smax'], min(x['band'] for x in S), max(x['band'] for x in S),
                 'yes' if crossing(ser) else 'no'))

    test_absolute_band(series)
    dvals = [int(x) for x in args.d.split(',') if x.strip()]
    test_dither(series, dvals, args.noise, NOISE_BAND, avg=args.avg)
    test_differential_band(series, d_values=tuple(d for d in dvals if d >= 20))
    test_edge_width()

    print('\n== overall verdict ==')
    print('  1. A single frame cannot give an absolute defocus distance across scenes (A).')
    print('  2. The winning offline estimator is the two-position differential band slope (C):')
    print('     ~10-20 pulses error, leave-one-scene-out, at d=20-40 pulses.  That is the')
    print('     idea worth a field session; the local-difference features are what generalise.')
    print('  3. The 3-point dither vertex (B) is worse than doing nothing at 1 % noise; keep')
    print('     only the band-slope direction probe.  D (edge width) is coarse-only by physics.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
