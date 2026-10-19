#!/usr/bin/env python3
"""af_lab: candidate autofocus algorithms and an evaluation harness for the Canon EOS M
ML project (crop_rec "AF drive (MPU)"), built on top of af_replay.py.

  python3 af_lab.py [--logs <dir>] [--trials N] [--seed S] [--report path]

af_replay.py is imported as a library (it is not modified).  This lab uses the same
fitted curve pool, noise, move timing and Cam/Ctl API, and adds:

  (a) G  continuous sweep with predictive braking (log-parabola + CUSUM fallback)
  (b) H  Gaussian fit on a short coarse climb + jump to the vertex (improved D)
  (c) I  Brent/golden-section refinement inside a coarse bracket
  (d) J  two-sided linear slope probe (direction/bracket from the slope, not a fixed probe)
  (e) K  DFD-style relative-blur coarse estimator (calibrated sigma_s) + verification
  (f) local_v2  per-scene high-water-mark reference instead of the ratcheting Auto-refocus
      reference (see below)
  (g) local_v0/v1  mirror of the current afd_run_local and of the handover 4.1 fix
plus baselines A/B/C/F from af_replay.

The local-refocus experiment models the reference bookkeeping of the Refocus test as a
scenario parameter, as found in crop_rec.c: after each landing the Auto-refocus reference
is re-set from the new settled sample, so it ratchets down toward soft landings.  Three
scenarios are simulated: reference fixed at the true scene peak, and reference *= 0.90 or
0.85 after every landing (the logs show 115k -> 95k -> 86k -> 77k, i.e. about x0.9 per
landing).  Success is always absolute: final true s >= 90 % of the true peak.

This is a simulation, not camera data.  See REPORT_LAB.md for the assumptions.
"""
import argparse
import glob
import math
import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import af_replay as base                                   # noqa: E402

# ========================================================================================
# camera: same lens as af_replay.Cam, plus a measurement-gain hook, calibration errors
# and a record of every s reading (the Refocus test / firmware keeps best_s).
# ========================================================================================

class LabCam(base.Cam):
    """af_replay.Cam + a per-zoom calibration error on sigma_s / floor / peak, an optional
    measurement-gain drift, and self.obs = every s reading (for per-scene maxima)."""

    def __init__(self, P, rng, curve, x0, noise_mult=1.0, erosion=None, seed='',
                 stop_dead=False):
        super().__init__(P, rng, curve, x0, noise_mult)
        self.stop_dead = stop_dead
        cr = random.Random('%s/cal' % seed)
        # a DFD implementation needs a calibrated sigma_s per zoom (+-10 %, like sig_cal for g)
        self.sig_s_cal = curve['sig_s'] * math.exp(cr.gauss(0, 0.10))
        self.bs_known = base.B_S * math.exp(cr.gauss(0, 0.10))
        true_peak = base.B_S * (1.0 + curve['ab_s'])
        self.zoom_peak = true_peak * math.exp(cr.gauss(0, 0.15))     # bench value for this zoom
        self.erosion = erosion
        self.obs = []
        self._drop, self._t0, self._t1 = 0.0, 0.0, 1.0
        if erosion:
            self._drop = erosion['drop']
            self._t0 = erosion.get('t0', 300.0)
            self._t1 = self._t0 + erosion.get('dur', 4000.0)

    def gain(self):
        if not self.erosion:
            return 1.0
        if self.erosion.get('mode', 'ramp') == 'step':
            return 1.0 if self.t < self._t0 else 1.0 - self._drop
        f = (self.t - self._t0) / max(self._t1 - self._t0, 1.0)
        return 1.0 - self._drop * min(max(f, 0.0), 1.0)

    def stop(self, h):
        """The field fact: (9,3,1) may not halt a (9,1) move at all.  With stop_dead the lens
        completes the whole move and the algorithm is told it travelled the full distance."""
        if self.stop_dead:
            self._finish(h)
            return h['n']
        return super().stop(h)

    def _read(self, x, k=1):
        s, g = super()._read(x, k)
        a = self.gain()
        s, g = s * a, g * a
        self.obs.append(s)
        return s, g


# ========================================================================================
# numeric helpers (stdlib only)
# ========================================================================================

def quad_vertex_log(xs, ys):
    """vertex of a parabola fitted to ln(y); None if not concave. -> (x_vertex, y_vertex)"""
    if len(xs) < 3:
        return None
    try:
        ly = [math.log(max(v, 1e-9)) for v in ys]
    except ValueError:
        return None
    rows = [[1.0, x, x * x] for x in xs]
    try:
        c0, b, a = base.lstsq(rows, ly)
    except (IndexError, ZeroDivisionError):
        return None
    if a >= -1e-9:
        return None
    v = -b / (2.0 * a)
    ex = c0 - b * b / (4.0 * a)
    if not (-30.0 < ex < 30.0) or not (-1e6 < v < 1e6):
        return None
    return v, math.exp(ex)


def fit_gauss_floor(pos, y, bmin=None, bmax=None, sig_lo=8.0, sig_hi=110.0):
    """Grid (p*, sigma) fit of y = B + A*exp(-(p-p*)^2/(2 sig^2)) with few points.
    B free in [0.4*min, min(y)] by default; A by linear LS.  Returns dict or None."""
    n = len(pos)
    if n < 3:
        return None
    ymin, ymax = min(y), max(y)
    if ymax <= 0:
        return None
    bmin = 0.40 * ymin if bmin is None else bmin
    bmax = ymin if bmax is None else bmax
    sst = sum((v - sum(y) / n) ** 2 for v in y)
    if sst <= 0:
        return None
    sigs = [sig_lo * (sig_hi / sig_lo) ** (i / 15.0) for i in range(16)]
    step = max(2.0, (max(pos) - min(pos)) / 40.0)
    best = None
    p = min(pos) - 2.5 * step
    while p <= max(pos) + 2.5 * step:
        for sg in sigs:
            r = base._gfit_at(pos, y, p, sg, bmin, bmax)
            if r and (best is None or r[0] < best[0]):
                best = (r[0], p, sg, r[1], r[2])
        p += step
    if best is None:
        return None
    sse, p, sg, B, A = best
    r2 = 1.0 - sse / sst
    return dict(p=p, sig=sg, B=B, A=A, ab=A / B if B else float('inf'), r2=r2, n=n,
                pred=B + A)


def _probe(c, pos, k=2):
    """Move to absolute commanded position pos and measure; returns (pos, s)."""
    need = pos - c.pos
    if abs(need) < 0.5:
        s = c.cam.measure(k)[0]
    else:
        s = c.mv(int(round(need)), k)[0]
    return c.pos, s


def climb_points(c, d, step, max_steps, start_s):
    """Same-direction coarse climb. -> (pts, ended_without_drop).  pts = [(pos, s)]"""
    P = c.P
    pts = [(c.pos, start_s)]
    best_s = start_s
    below = 0
    ended = True
    for _ in range(max_steps):
        if c.cam.travel + step > P.max_travel:
            break
        s = c.mv(d * step, 1)[0]
        pts.append((c.pos, s))
        if s > best_s:
            best_s, below = s, 0
        elif s * 100 < best_s * 85:
            below += 1
        else:
            below = 0
        if below >= 2:
            ended = False
            break
        # flat floor over the first 3 steps: no local signal, stop walking away
        if len(pts) >= 3 and max(p[1] for p in pts[1:]) < 1.03 * start_s:
            ended = False
            break
    return pts, ended


def get_bracket(c, d0, step, max_steps, start_s):
    """Coarse climb until the peak is bracketed.  Tries the other direction once when the
    first point is the best (peak behind). -> dict(state, pts, ib, d, lo, hi)"""
    d = d0
    pts, ib, ended = [], 0, True
    reversed_once = False
    for _ in range(3):
        pts, ended = climb_points(c, d, step, max_steps, start_s)
        ib = max(range(len(pts)), key=lambda i: pts[i][1])
        ytop = pts[ib][1]
        ymin = min(p[1] for p in pts)
        if len(pts) <= 3 and ytop < 1.03 * start_s:
            # completely flat over the first 2 steps: do not keep walking away
            return dict(state='flat', pts=pts, ib=ib, d=d)
        prominent = ytop >= 1.10 * ymin and ytop >= 1.05 * start_s
        if 0 < ib < len(pts) - 1 and prominent:
            return dict(state='bracket', pts=pts, ib=ib, d=d,
                        lo=pts[ib - 1][0], hi=pts[ib + 1][0])
        # no usable bracket yet: try the other direction once (the peak may be behind)
        if not reversed_once:
            d = -d
            start_s = max(p[1] for p in pts)
            reversed_once = True
            continue
        if not prominent:
            return dict(state='flat', pts=pts, ib=ib, d=d)
        return dict(state='ahead', pts=pts, ib=ib, d=d)
    return dict(state='ahead', pts=pts, ib=ib, d=d)


def brent_refine(c, lo, hi, max_iter=6, d_hint=1):
    """Golden-section search on the noisy s, inside [lo, hi].  Returns (best_pos, best_s)."""
    if hi - lo < 2.0:
        p, s = _probe(c, 0.5 * (lo + hi))
        return p, s
    gr = (math.sqrt(5.0) - 1.0) / 2.0
    a, b = float(lo), float(hi)
    x1 = b - gr * (b - a)
    x2 = a + gr * (b - a)
    p, f1 = _probe(c, x1)
    best = (p, f1)
    p, f2 = _probe(c, x2)
    if f2 > best[1]:
        best = (p, f2)
    for _ in range(max_iter):
        if f1 > f2:
            b, f2, x2 = x2, f1, x1
            x1 = b - gr * (b - a)
            p, f1 = _probe(c, x1)
            if f1 > best[1]:
                best = (p, f1)
        else:
            a, f1, x1 = x1, f2, x2
            x2 = a + gr * (b - a)
            p, f2 = _probe(c, x2)
            if f2 > best[1]:
                best = (p, f2)
    return best


def goto_land(c, target, ref, d, max_steps=10):
    """Final approach to target, then a *bounded* stepped feedback climb (pass-A style) if the
    first reading is low.  The approach is one relative move (not afd_goto's back+approach):
    with unknown reversal offsets an absolute goto adds two more reversals and can land a
    whole R away from where the algorithm believes it is."""
    P = c.P
    delta = int(round(target - c.pos))
    if abs(delta) >= 3:
        s = c.mv(delta, 2)[0]
    else:
        s = c.cam.measure(2)[0]
    best_s = s
    if s * 100 >= ref * 90:
        return s
    start = c.pos - d * P.RT_BACK
    if abs(start - c.pos) > 1:
        c.mv(start - c.pos, 0)
    pm, pi = s, -1
    for i in range(max_steps):
        s2 = c.mv(d * P.FINE, 2)[0]
        if s2 > pm:
            pm, pi, best_s = s2, i, s2
        if s2 * 100 >= ref * 90:
            return s2
        if i > pi and s2 * 100 < pm * 80:
            break
    return best_s


def sweep_fallback_dir(cam, P, d0=1):
    """Full sweep + stepped retrace, starting in the known/guessed direction d0.  This is
    base.sweep_find with a non-fixed initial direction, so a fallback does not first burn a
    400-pulse sweep the wrong way."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    d = d0 if d0 else 1
    L = 400
    for k in range(6):
        if cam.travel + L > P.max_total:
            break
        if k:
            s0, g0 = cam.measure(1)
            c.last_g = g0
        res = base.sweep_move(c, d, L, s0, g0)
        if res['found']:
            base.retrace_chain_from_pk(c, res, d, 80, 160)
            return res
        if res['wrong']:
            d = -d
        elif res['im'] >= 1 and res['smax'] >= 1.15 * res['floor']:
            pass
        else:
            d, L = -d, L + 400
    s0, g0 = cam.measure(2)
    c.last_g = g0
    res, dd = base.sweep_find(c, s0, g0)
    if res is not None:
        base.retrace_chain_from_pk(c, res, dd, 80, 160)
    return res


def sweep_fallback(cam, P, d0=1):
    return sweep_fallback_dir(cam, P, d0)


# ========================================================================================
# (a) G: continuous sweep with predictive braking
# ========================================================================================

def sweep_brake(c, d, L, s0, g0, nfit=5):
    """One long move sampled every frame; stop when a log-parabola through the last nfit
    samples puts the vertex inside the coast window, with a CUSUM/negative-slope fallback.
    Returns dict(found, wrong, climb_end, pk, ref, floor, pts)."""
    P, cam = c.P, c.cam
    h = cam.begin(d * L)
    p0 = c.pos
    pts = [(p0, s0, g0)]
    runmax, floor, below = s0, s0, 0     # floor = the start reading (the far tail), not a noise minimum
    wrong = climb_end = stopped = False
    brake_lat = getattr(P, 'brake_lat', 60.0)
    v = cam.v_alg
    fit_pk = fit_ref = None
    while True:
        r = cam.next_sample(h)
        if r is None:
            c.pos += d * L
            break
        tau, s, g = r
        pos = p0 + cam.est_img_pos(h, tau)
        pts.append((pos, s, g))
        floor = min(floor, s)
        if s > runmax:
            runmax, below = s, 0
        elif s * 100 < runmax * 88:
            below += 1
        else:
            below = 0
        # peak behind: the start reading stays the best and both metrics have fallen
        if len(pts) >= 5 and (pts[-1][1] + pts[-2][1]) * 100 < 2 * s0 * 92 \
                and (pts[-1][2] + pts[-2][2]) * 100 < 2 * g0 * 96:
            wrong = True
        if wrong:
            c.pos += cam.stop(h)
            stopped = True
            break
        if len(pts) >= nfit and runmax >= 1.18 * floor and s >= 0.60 * runmax:
            sel = pts[-nfit:]
            xs = [q[0] for q in sel]
            ys = [q[1] for q in sel]
            if all(xs[i + 1] > xs[i] + 1.0 for i in range(len(xs) - 1)):
                vv = quad_vertex_log(xs, ys)
                if vv is not None:
                    vx, vpeak = vv
                    ahead = vx - pos
                    coast = v * brake_lat
                    # the parabola must actually describe the last samples (no noise blow-up)
                    ok_res = True
                    try:
                        c0, b, a = base.lstsq([[1.0, x, x * x] for x in xs],
                                              [math.log(max(yy, 1e-9)) for yy in ys])
                        maxrel = max(abs(math.exp(c0 + b * x + a * x * x) / yy - 1.0)
                                     for x, yy in zip(xs, ys))
                        ok_res = maxrel < 0.10
                    except (ValueError, ZeroDivisionError):
                        ok_res = False
                    if ok_res and -2.0 * v * P.frame <= ahead <= coast + 0.5 * v * P.frame \
                            and 1.02 * runmax <= vpeak <= 1.20 * runmax \
                            and vpeak >= 1.05 * floor:
                        fit_pk, fit_ref = vx, vpeak
                        c.pos += cam.stop(h)
                        stopped = True
                        break
        if below >= 2 and runmax >= 1.15 * floor and len(pts) >= 5:
            c.pos += cam.stop(h)          # fallback: CUSUM / negative slope
            stopped = True
            break
    # settle and verify once after a stop (the firmware samples after the stop too)
    settled = None
    if stopped and not wrong:
        settled = cam.measure(2)[0]
        runmax = max(runmax, settled)
        # a false brake on a still-rising leg: the settled sharpness is far below the
        # predicted peak -> treat as "still climbing", keep sweeping
        if fit_pk is not None and settled * 100 < fit_ref * 80:
            return dict(found=False, wrong=False, climb_end=True, pk=None, ref=runmax,
                        floor=floor, pts=pts, smax=runmax, im=0)
    n = len(pts)
    if wrong:
        return dict(found=False, wrong=True, climb_end=False, pk=None, ref=s0,
                    floor=floor, pts=pts, smax=runmax, im=0)
    # did we see a peak? (a lower sample after the maximum, or a predictor stop)
    ib = max(range(n), key=lambda i: pts[i][1])
    found = (runmax >= 1.15 * floor) and ((ib >= 1 and (ib < n - 1 or stopped)) or fit_pk is not None)
    if not found:
        return dict(found=False, wrong=False, climb_end=(ib == n - 1), pk=None,
                    ref=runmax, floor=floor, pts=pts, smax=runmax, im=ib)
    if fit_pk is not None:
        pk, ref = fit_pk, max(fit_ref, runmax)
    else:
        pk = pts[ib][0]
        ref = runmax
        if 0 < ib < n - 1:
            vv = quad_vertex_log([pts[ib - 1][0], pts[ib][0], pts[ib + 1][0]],
                                 [pts[ib - 1][1], pts[ib][1], pts[ib + 1][1]])
            if vv is not None and pts[ib - 1][0] <= vv[0] <= pts[ib + 1][0]:
                pk = vv[0]
                ref = max(runmax, min(vv[1], 1.12 * runmax))
    return dict(found=True, wrong=False, climb_end=False, pk=pk, ref=ref,
                floor=floor, pts=pts, smax=runmax, im=ib)


def algo_G(cam, P):
    """G: continuous sweep with predictive braking, then the stepped feedback retrace."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    d, L = 1, 400
    for k in range(6):
        if cam.travel + L > P.max_total:
            break
        if k:
            s0, g0 = cam.measure(1)
            c.last_g = g0
        res = sweep_brake(c, d, L, s0, g0)
        if res['found']:
            base.retrace_chain(c, res['pk'], res['ref'], d, 80, 160)
            return
        if res['wrong']:
            d = -d
        elif res.get('im', 0) >= 1 and res['smax'] >= 1.15 * res['floor']:
            pass                              # still climbing at the end: continue
        else:
            d, L = -d, L + 400
    sweep_fallback(cam, P)


# ========================================================================================
# (b) H: Gaussian fit + vertex jump;  (c) I: Brent/golden refinement
# ========================================================================================

def _bracket_fit(pts, ib):
    sel = pts[max(0, ib - 2):ib + 3]
    xs = [p[0] for p in sel]
    ys = [p[1] for p in sel]
    return fit_gauss_floor(xs, ys)


def algo_H(cam, P):
    """H: short coarse climb -> Gaussian fit -> jump to the vertex + one verification;
    confidence gate (R^2 / prominence / sigma) with Brent fallback."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    br = get_bracket(c, 1, 40, 8, s0)
    if br['state'] != 'bracket':
        sweep_fallback(cam, P, br.get('d', 1) if br['state'] == 'ahead' else 1)
        return
    if br['state'] == 'flat':
        sweep_fallback(cam, P)
        return
    ib, pts, d = br['ib'], br['pts'], br['d']
    fit = _bracket_fit(pts, ib)
    gate = (fit is not None and fit['r2'] >= 0.90 and fit['ab'] >= 0.20
            and 8.0 <= fit['sig'] <= 110.0
            and br['lo'] - 40.0 <= fit['p'] <= br['hi'] + 40.0
            and fit['pred'] >= 1.05 * max(p[1] for p in pts))
    if gate:
        vertex = fit['p']
        delta = int(round(vertex - c.pos))                     # one relative move
        s_v = c.mv(delta, 2)[0] if abs(delta) >= 3 else c.cam.measure(2)[0]
        ymax = max(p[1] for p in pts)
        # the fitted peak value is often optimistic: judge mostly on the observed maximum
        if s_v * 100 >= fit['pred'] * 78 and s_v >= 0.90 * ymax:
            return                                             # verified: stay here
    # fallback: Brent inside the bracket (golden section), then a bounded landing
    best_pos, best_s = brent_refine(c, br['lo'], br['hi'], max_iter=5, d_hint=d)
    ref = max(best_s, max(p[1] for p in pts))
    goto_land(c, best_pos, ref, d)


def algo_I(cam, P):
    """I: coarse bracket + Brent/golden-section refinement as a standalone search."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    br = get_bracket(c, 1, 40, 10, s0)
    if br['state'] != 'bracket':
        sweep_fallback(cam, P, br.get('d', 1) if br['state'] == 'ahead' else 1)
        return
    best_pos, best_s = brent_refine(c, br['lo'], br['hi'], max_iter=6, d_hint=br['d'])
    ref = max(best_s, max(p[1] for p in br['pts']))
    goto_land(c, best_pos, ref, br['d'])
    return


# ========================================================================================
# (d) J: two-sided linear slope probe
# ========================================================================================

def algo_J(cam, P):
    """J: measure at -d, 0, +d; direction and bracket from the slope instead of the fixed
    +100 probe.  A peak inside the probe span goes to Brent, a one-sided slope to the
    coarse climb."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    p1 = 2 * P.FINE                                    # +-40 pulses
    sp = c.mv(p1, 1)[0]                                # at +p1
    sm = c.mv(-2 * p1, 1)[0]                           # at -p1
    better_p = sp * 100 > s0 * 105
    better_m = sm * 100 > s0 * 105
    if not better_p and not better_m:
        if max(sp, sm) * 100 < s0 * 110:
            # no slope anywhere near the start: not a local peak, do the full sweep
            sweep_fallback(cam, P)
            return
        # no side better but there is contrast: the peak is probably inside +-p1
        best_pos, best_s = brent_refine(c, -2 * p1, 2 * p1, max_iter=4, d_hint=1)
        goto_land(c, best_pos, max(best_s, s0), 1)
        return
    d = 1 if (better_p and (not better_m or sp >= sm)) else -1
    if better_p and better_m:
        # both sides better than the start: peak between them -> local bracket
        best_pos, best_s = brent_refine(c, -p1, p1, max_iter=4, d_hint=1)
        goto_land(c, best_pos, max(best_s, s0), d)
        return
    # one-sided slope: coarse climb from the current position (-p1) in direction d
    br = get_bracket(c, d, 40, 10, max(s0, sp, sm))
    if br['state'] == 'bracket':
        best_pos, best_s = brent_refine(c, br['lo'], br['hi'], max_iter=5, d_hint=br['d'])
        ref = max(best_s, max(p[1] for p in br['pts']))
        goto_land(c, best_pos, ref, br['d'])
    else:
        sweep_fallback(cam, P, br.get('d', d) if br['state'] == 'ahead' else 1)


# ========================================================================================
# (e) K: DFD-style relative-blur coarse estimator
# ========================================================================================

def algo_K(cam, P, D=60):
    """K: two s samples at known positions (0, D), invert the calibrated Gaussian shape to
    p*, jump there, verify; on a failed verification try the mirrored side and then fall
    back to a full sweep."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    s1 = c.mv(D, 2)[0]
    Bk = getattr(cam, 'bs_known', base.B_S)
    sig = getattr(cam, 'sig_s_cal', None)
    if sig is None:
        sweep_fallback(cam, P)
        return
    if s0 <= Bk * 1.005 or s1 <= Bk * 1.005:
        # no usable contrast above the floor: DFD cannot estimate
        sweep_fallback(cam, P)
        return
    r = (s0 - Bk) / (s1 - Bk)
    if r <= 0.01:
        sweep_fallback(cam, P)
        return
    lnr = math.log(r)
    p = 0.5 * D + sig * sig * lnr / D
    # clamp the jump to a sane horizon around the two samples
    p = min(max(p, -3.0 * sig), D + 3.0 * sig)
    e1 = math.exp(-((D - p) ** 2) / (2.0 * sig * sig))
    A = (s1 - Bk) / max(e1, 1e-6)
    pred = Bk + A
    if A / max(Bk, 1e-9) < 0.25:
        # too little prominence above the floor for a calibrated DFD inversion
        sweep_fallback(cam, P)
        return
    target = p
    d = base.sgn(target - c.pos) or 1
    s_v = goto_land(c, target, max(pred, s1), d)
    if s_v * 100 >= pred * 84 and s_v >= 0.90 * max(s0, s1):
        return
    # side ambiguity: try the mirror position about the midpoint of the two samples
    mir = D - p
    d2 = base.sgn(mir - c.pos) or 1
    s_m = goto_land(c, mir, max(pred, s1), d2)
    if s_m * 100 >= pred * 84 and s_m >= 0.90 * max(s0, s1):
        return
    sweep_fallback(cam, P)


# ========================================================================================
# local Refocus-test experiment: current logic, handover 4.1 fix, high-water-mark ref
# ========================================================================================

def _land_steps(c, pmax, ref, d, back):
    """afd_land_steps: retrace 1 (92 %), retrace 2 (80 %), else walk to the best point."""
    P = c.P
    start = pmax - d * back
    hit, pm, bidx = base.retrace(c, 1, 92, ref, start, d,
                                 int(back // P.FINE) + P.RT_SLACK + P.RT_EXTRA // P.FINE)
    if not hit:
        h2, pm2, b2 = base.retrace(c, 2, 80, ref, start, d,
                                   int((back + P.RT_BACK2 - P.RT_BACK) // P.FINE)
                                   + P.RT_SLACK + 2 * P.RT_EXTRA // P.FINE)
        bidx = max(bidx, b2)
        if h2:
            hit, pm = True, pm2
    if not hit:
        c.mv(start - c.pos, 0)
        for _ in range(max(bidx, 0) + 1):
            c.mv(d * P.FINE, 2)
        return c.cam.measure(2)[0]
    return pm


def _local_ref(ctx):
    return max(ctx['ref'], ctx.get('hwm', 0.0))


def _local_b(cam, P):
    base.algo_B(cam, P)
    return dict(fallback=True)


def local_run(cam, P, ctx, variant):
    """Mirror of afd_run_local (variant 0) and the fixes from doc/EOSM-AF-HANDOVER.md 4.1
    (variant 1) and 4.2 (variant 2: per-scene high-water-mark reference)."""
    c = base.Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    if variant == 2:
        ref = max(ctx['ref'], ctx.get('hwm', 0.0))
    else:
        ref = ctx['ref']
    fine = P.FINE
    p1 = 2 * fine
    sp = c.mv(p1, 1)[0]
    sm = c.mv(-2 * p1, 1)[0]
    better_p = sp * 100 > s0 * 105
    better_m = sm * 100 > s0 * 105
    d = 0 if (not better_p and not better_m) else (1 if (better_p and (not better_m or sp >= sm)) else -1)
    smax = max(s0, sp, sm)
    # far from the (same zoom) reference: full search, like the current code
    if smax * 100 < ref * 50:
        return _local_b(cam, P)
    if not d:
        # ---------------- no side improves ----------------
        if variant >= 1:
            if ctx['drop'] or s0 * 100 < ref * 92:
                # handover 4.1: probe wider (+-4*fine) once; the local branch is then
                # abandoned (fall back to the full sweep) instead of taking the
                # "in focus" exit, which is what accepted the soft pictures in the field.
                c.mv(-2 * fine, 1)                   # to -4*fine
                c.mv(8 * fine, 1)                    # to +4*fine
                return _local_b(cam, P)
            # mild trigger and still close to the reference: accept in place
            c.mv(p1, 1)                              # back to the start reading
            return dict(fallback=False)
        # variant 0: current code - wide gate, then accept in place
        if smax * 100 < ref * 70:
            return _local_b(cam, P)
        se = c.mv(p1, 1)[0]                          # back to the start
        if se * 100 < s0 * 85:
            se = _land_steps(c, c.pos, s0, 1, P.RT_BACK)
        return dict(fallback=False)
    # ---------------- climb on the better side ----------------
    fmax = sm
    if d > 0:
        fmax = c.mv(2 * p1, 1)[0]                    # from -p1 to +p1
    pmax = c.pos
    since = below = steps = 0
    for _ in range(8):
        s = c.mv(d * fine, 2)[0]
        steps += 1
        if s > fmax:
            fmax, pmax, since, below = s, c.pos, 0, 0
            continue
        since += 1
        below = below + 1 if s * 100 < fmax * 90 else 0
        if below >= 2:
            break
    if since == 0 and steps >= 8:
        return _local_b(cam, P)
    if fmax * 100 < ref * 50:
        return _local_b(cam, P)
    land = _land_steps(c, pmax, fmax, d, P.RT_BACK)
    if not land:
        return _local_b(cam, P)
    return dict(fallback=False)


def local_keep(cam, P, ctx, variant):
    cam.measure(2)
    return dict(fallback=False)


LOCAL_ALGOS = [
    ('local_v0', 'current afd_run_local logic (reference from bookkeeping)', local_run),
    ('local_v1', 'handover 4.1: wider +-4*fine probe, no in-focus exit on drop path', local_run),
    ('local_v2', 'v1 + per-scene high-water-mark reference (handover 4.2)', local_run),
    ('keep', 'no search: trust the previous landing', local_keep),
    ('B', 'full sweep + retrace on every trigger', None),
]


def run_sequence(variant, P, spec, drift_lo, drift_hi, ref_mode, K=5, ratchet=0.866):
    """One Refocus-test-like sequence: K triggers of a tracked, mildly blurred scene with a
    per-scene focus drift between triggers.

    ref_mode: 'fixed'  reference stays at the true scene peak
              'ratchet_field' / 'ratchet85'  reference *= the measured factor (~0.866) / 0.85
              after every landing
    The *bookkeeping* reference is a scenario parameter (from crop_rec aref_mpu_finish/
    settle: the reference is re-set from the new landing), not an algorithm choice.
    """
    seed = spec['seed']
    rng = random.Random('%s/seq' % seed)
    cam = LabCam(P, rng, spec['curve'], 0.0, spec.get('noise_mult', 1.0), None, seed)
    peak0 = cam.s_true(0.0)
    cr = random.Random('%s/ref' % seed)
    peak_obs = peak0 * (1.0 + cr.gauss(0, P.noise))
    ref = peak_obs
    hwm = peak_obs
    rows, refs = [], [ref]
    for k in range(K):
        mag = rng.uniform(drift_lo, drift_hi)
        delta = mag * (1.0 if rng.random() < 0.5 else -1.0)
        cam.x -= delta                                  # the focus plane moves, the lens does not
        trig = cam.s_true(cam.x) * cam.gain() * (1.0 + rng.gauss(0, cam.nf))
        trig_pct = 100.0 * trig / ref if ref > 0 else 100.0
        drop_path = trig_pct < 95.0
        eff_ref = max(ref, hwm) if variant == 'local_v2' else ref
        ctx = dict(ref=ref, hwm=hwm, zoom_base=getattr(cam, 'zoom_peak', ref),
                   drop=drop_path, trig=trig_pct)
        t0, m0, o0 = cam.t, cam.moves, len(cam.obs)
        timeout = False
        try:
            if variant == 'B':
                base.algo_B(cam, P)
                info = dict(fallback=True)
            elif variant == 'keep':
                info = local_keep(cam, P, ctx, 0)
            else:
                info = local_run(cam, P, ctx, int(variant[-1]))
        except base.AFTimeout:
            timeout = True
            info = dict(fallback=True)
        q = cam.quality()
        newobs = cam.obs[o0:] or [trig]
        land = base.med(newobs[-3:])
        q_ref = eff_ref / peak_obs if peak_obs > 0 else 1.0
        rows.append(dict(ok=q >= 0.9, soft=(0.75 <= q < 0.9), hard=q < 0.75, q=q,
                         q_vs_ref=q / q_ref if q_ref > 0 else q,
                         trig=trig_pct, drop=drop_path,
                         t=(cam.t - t0) / 1000.0, moves=cam.moves - m0,
                         fallback=info.get('fallback', False),
                         ref_in=eff_ref))
        # bookkeeping: new reference from the landing vs a per-scene high-water mark
        if ref_mode == 'fixed':
            ref = peak_obs
        elif ref_mode == 'ratchet_field':
            ref = ratchet * ref
        else:
            ref = 0.85 * ref
        hwm = max(hwm, land)                            # high-water mark: MAX verified landing
        refs.append(max(ref, hwm) if variant == 'local_v2' else ref)
    return dict(rows=rows, refs=refs, final_q=rows[-1]['q'], hwm=hwm / peak0)


# ========================================================================================
# algorithms registry
# ========================================================================================

ALGOS = [
    ('A', 'baseline: probe + coarse g + pass A + retrace', base.algo_A),
    ('B', 'continuous sweep + stepped feedback retrace', base.algo_B),
    ('C', 'continuous sweep + continuous retrace (stop on threshold)', base.algo_C),
    ('F', 'baseline with predictive stop in the stepped climb', base.algo_F),
    ('G', 'sweep with predictive braking (log-parabola vertex + CUSUM)', algo_G),
    ('H', 'Gaussian fit of a coarse bracket + vertex jump + verification', algo_H),
    ('I', 'Brent/golden-section inside a coarse bracket', algo_I),
    ('J', 'two-sided linear slope probe -> direction/bracket', algo_J),
    ('K', 'DFD-style calibrated relative-blur jump + verification', algo_K),
]
ALGO_FN = {k: f for k, d, f in ALGOS}


# ========================================================================================
# real-log checks: AFSWEEP geometry, AFSTOP stop evidence, AFRTEST reference erosion
# ========================================================================================

def afsweep_summary(data, fits):
    """What the real AFSWEEP.LOG says about the sweep geometry, and how af_replay uses it."""
    files = [fp for fp, head, sweeps in data.get('sweeps', [])]
    sw = [x for x in fits if x['kind'] == 'AFSWEEP' and x['fs'] is not None]
    ok = [x for x in sw if x['ok_s']]
    # the card8 and sweeponly copies are identical: dedupe by the fitted shape
    seen, uniq = set(), []
    for x in sw:
        key = (round(x['fs']['p'], 0), round(x['fs']['sig'], 1), round(x['fs']['ab'], 2))
        if key not in seen:
            seen.add(key)
            uniq.append(x)
    uok = [x for x in uniq if x['ok_s']]
    stepped = [x for x in fits if x['kind'] in ('AFDRIVE', 'AFCALIB')
               and x['ok_s'] and x['step'] <= 60]
    return dict(files=len(files), series=len(uniq), ok=len(uok),
                sig=[x['fs']['sig'] for x in uok],
                step_sig=[x['fs']['sig'] for x in stepped])


def parse_afstop(logs):
    """The real stop test: a planned stop time vs the completion time that followed."""
    out = []
    for fp in glob.glob(os.path.join(logs, '**', 'AFSTOP.LOG'), recursive=True):
        plans, stops = {}, {}
        for line in open(fp, errors='replace'):
            m = re.search(r'# stop_plan k=(\d+) at=(\d+)', line)
            if m:
                plans[int(m.group(1))] = int(m.group(2))
            m = re.search(r'# stop k=(\d+) t_stop=(\d+) t_done=(\d+)', line)
            if m:
                stops[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
        if plans:
            out.append(dict(path=fp, n=len(plans),
                            rows=[(k, plans[k], stops.get(k)) for k in sorted(plans)]))
    return out


def parse_afrtest(logs):
    """The real Refocus-test reference sequences (RT_REF) and trial outcomes (RT_TRIAL)."""
    files = []
    for fp in glob.glob(os.path.join(logs, '**', 'AFRTEST.LOG'), recursive=True):
        refs, trials = [], []
        for line in open(fp, errors='replace'):
            if line.startswith('# RT_REF'):
                m = re.search(r'land_s=(\d+) ref_s=(\d+)', line)
                if m:
                    refs.append((int(m.group(1)), int(m.group(2))))
            elif line.startswith('# RT_TRIAL'):
                m = re.search(r'final_pct=(\d+).*ok=(\d+)', line)
                mr = re.search(r'runs=(\d+)', line)
                if m:
                    trials.append((int(m.group(1)), int(m.group(2)),
                                   int(mr.group(1)) if mr else 1))
        ratios = [refs[i + 1][1] / refs[i][1] for i in range(len(refs) - 1)]
        if refs:
            files.append(dict(path=fp, refs=refs, trials=trials, ratios=ratios))
    allr = [r for f in files for r in f['ratios']]
    return dict(files=files, ratios=allr,
                ratchet=base.med(allr) if allr else 0.87)


# ========================================================================================
# Monte Carlo harness
# ========================================================================================

def make_specs(cls, n, seed, pool, curve_src='real', noise_mult=1.0, erosion=None,
               stop_dead=False):
    specs = base.make_specs(cls, n, seed, pool, curve_src, noise_mult)
    for sp in specs:
        sp['erosion'] = erosion
        sp['stop_dead'] = stop_dead
    return specs


def run_trial(algo, P, spec):
    rng = random.Random(spec['seed'])
    cam = LabCam(P, rng, spec['curve'], spec['x0'], spec['noise_mult'],
                 spec.get('erosion'), spec['seed'], spec.get('stop_dead', False))
    timeout = False
    try:
        ALGO_FN[algo](cam, P)
    except base.AFTimeout:
        timeout = True
    q = cam.quality()
    return dict(ok=q >= 0.9, soft=(0.75 <= q < 0.9), hard=q < 0.75, q=q,
                t=min(cam.t, P.cap_ms) / 1000.0, moves=cam.moves,
                travel=cam.travel, timeout=timeout)


def run_cell(algo, P, classes, n, seed, pool, curve_src='real', noise_mult=1.0,
             erosion=None, p_override=None, stop_dead=False):
    out = []
    PP = p_override if p_override is not None else P
    for cls in classes:
        for sp in make_specs(cls, n, seed, pool, curve_src, noise_mult, erosion,
                             stop_dead=stop_dead):
            out.append(run_trial(algo, PP, sp))
    return out


def summarize(res):
    n = len(res)
    if not n:
        return dict(n=0, succ=float('nan'), soft=0.0, hard=0.0, t50=float('nan'),
                    t90=float('nan'), moves=float('nan'), travel=float('nan'),
                    to=0.0, q50=float('nan'))
    ts = [r['t'] for r in res]
    return dict(n=n,
                succ=100.0 * sum(r['ok'] for r in res) / n,
                soft=100.0 * sum(r['soft'] for r in res) / n,
                hard=100.0 * sum(r['hard'] for r in res) / n,
                t50=base.med(ts), t90=base.pct(ts, 90),
                moves=base.med([r['moves'] for r in res]),
                travel=base.med([r['travel'] for r in res]),
                to=100.0 * sum(r['timeout'] for r in res) / n,
                q50=base.med([r['q'] for r in res]))


def fmt_cell(c):
    return '%.0f%% / %.1f / %.1f' % (c['succ'], c['t50'], c['t90'])


def run_main(P, pool, n, seed, curve_src='real'):
    main, cold = {}, {}
    for algo, desc, fn in ALGOS:
        pooled = []
        for cls in ('warm', 'near', 'mid', 'far'):
            res = run_cell(algo, P, [cls], n, seed, pool, curve_src=curve_src)
            main[(algo, cls)] = summarize(res)
            if cls != 'warm':
                pooled += res
        cold[algo] = summarize(pooled)
    return main, cold


def run_sensitivity(P, pool, n, seed):
    R = {}
    R['R_list'] = [0, 30, 60, 100, 150]
    R['R'] = {}
    for algo, desc, fn in ALGOS:
        for r in R['R_list']:
            R['R'][(algo, r)] = summarize(
                run_cell(algo, P, ['near', 'mid'], n, seed, pool,
                         p_override=base.replace(P, R=float(r))))
    R['noise_list'] = [1.0, 3.0]
    R['noise'] = {}
    for algo, desc, fn in ALGOS:
        for nm in R['noise_list']:
            R['noise'][(algo, nm)] = summarize(
                run_cell(algo, P, ['near', 'mid'], n, seed, pool, noise_mult=nm))
    R['lat_list'] = [0, 60, 200]
    R['lat'] = {}
    for algo in ('B', 'C', 'G'):
        for lat in R['lat_list']:
            R['lat'][(algo, lat)] = summarize(
                run_cell(algo, P, ['near', 'mid'], n, seed, pool,
                         p_override=base.replace(P, stop_lat=float(lat))))
    # the field fact: (9,3,1) may not halt a (9,1) move at all
    R['dead'] = {}
    for algo in ('B', 'C', 'G'):
        R['dead'][algo] = summarize(
            run_cell(algo, P, ['near', 'mid'], n, seed, pool, stop_dead=True))
    return R


def summarize_local(seqs):
    rows = [r for s in seqs for r in s['rows'] if r['trig'] < 98.0]
    if not rows:
        return dict(n=0)
    n = len(rows)
    ref_ratio = [s['refs'][-1] / s['refs'][0] for s in seqs]
    ref_traj = []
    for i in range(len(seqs[0]['refs'])):
        ref_traj.append(base.med([s['refs'][i] / s['refs'][0] for s in seqs]))
    drop = [r for r in rows if r['drop']]
    mild = [r for r in rows if not r['drop']]
    return dict(n=n,
                succ=100.0 * sum(r['ok'] for r in rows) / n,
                soft=100.0 * sum(r['soft'] for r in rows) / n,
                hard=100.0 * sum(r['hard'] for r in rows) / n,
                q50=base.med([r['q'] for r in rows]),
                q10=base.pct([r['q'] for r in rows], 10),
                t50=base.med([r['t'] for r in rows]),
                t90=base.pct([r['t'] for r in rows], 90),
                moves=base.med([r['moves'] for r in rows]),
                fb=100.0 * sum(r['fallback'] for r in rows) / n,
                ref80_90=100.0 * sum(0.80 <= r['q_vs_ref'] < 0.90 for r in rows) / n,
                ref_lt80=100.0 * sum(r['q_vs_ref'] < 0.80 for r in rows) / n,
                abs80_90=100.0 * sum(0.80 <= r['q'] < 0.90 for r in rows) / n,
                drop_n=len(drop),
                drop_succ=100.0 * sum(r['ok'] for r in drop) / len(drop) if drop else float('nan'),
                drop_soft=100.0 * sum(r['soft'] for r in drop) / len(drop) if drop else float('nan'),
                mild_n=len(mild),
                mild_succ=100.0 * sum(r['ok'] for r in mild) / len(mild) if mild else float('nan'),
                mild_soft=100.0 * sum(r['soft'] for r in mild) / len(mild) if mild else float('nan'),
                ref_ratio=base.med(ref_ratio),
                ref_traj=ref_traj)


def run_local(P, pool, n, seed, ratchet=0.866):
    specs = {}
    for cls in ('warm', 'mid'):
        # curves/x0 seeds shared across every variant and reference scenario
        specs[cls] = base.make_specs('warm', n, '%s/local/%s' % (seed, cls), pool, 'real', 1.0)
    out = {}
    for variant, desc, fn in LOCAL_ALGOS:
        for cls, (lo, hi) in (('warm', (0.0, 50.0)), ('mid', (50.0, 150.0))):
            for mode in ('fixed', 'ratchet_field', 'ratchet85'):
                seqs = [run_sequence(variant, P, sp, lo, hi, mode, ratchet=ratchet)
                        for sp in specs[cls]]
                out[(variant, cls, mode)] = summarize_local(seqs)
    # reversal-offset sensitivity of the local search: R = 0..100, warm, ratchet_field
    out['R_list'] = [0, 30, 60, 100]
    for variant, desc, fn in LOCAL_ALGOS:
        if variant in ('keep',):
            continue
        for r in out['R_list']:
            PP = base.replace(P, R=float(r))
            seqs = [run_sequence(variant, PP, sp, 0.0, 50.0, 'ratchet_field', ratchet=ratchet)
                    for sp in specs['warm']]
            out[('R', variant, r)] = summarize_local(seqs)
    return out


# ========================================================================================
# report
# ========================================================================================

def build_report(args, meta, main, cold, sens, loc, main_syn=None, cold_syn=None):
    L = []
    w = L.append
    w('# AF lab report: candidate search algorithms')
    w('')
    w('Generated by `contrib/af_replay/af_lab.py` (`--trials %d --seed %s`, %.0f s).  '
      'Real fitted curve pool: %d curves from `%s`.'
      % (args.trials, args.seed, meta['elapsed'], meta['pool'], meta['logs']))
    w('')
    w('**This is a simulation, not camera data.**  The curve shapes, noise, move timing and '
      'the `Cam`/`Ctl` API are those of `af_replay.py` (fitted from the real logs); the lens '
      'model (reversal offset, sweep speed, stop latency) and the Auto-refocus reference '
      'bookkeeping are assumed.  Treat the numbers as a ranking of structures and the '
      'sensitivity tables as bounds, not as predicted camera numbers.  See the last section.')
    w('')
    w('The **real-curve** tables are the primary result; the same algorithms are also run on '
      'the synthetic curve family as a secondary check (section 2b), so a structure that only '
      'wins on one curve family is visible as such.')
    w('')
    w('## 1. Method')
    w('')
    w('**Reused from `af_replay.py` (imported as a library, not modified).**  Fitted '
      '`B + A*exp(-(p-p*)^2/(2 sigma^2))` curve pool, per-move timing and overheads, '
      'multiplicative reading noise (sd %.2f %% per reading, pair = /sqrt(2)), relative '
      'pulse moves with a uniform reversal offset in +-R (R = %g), continuous moves with one '
      'LV frame per %.0f ms, %.2f pulses/ms after a %.0f ms start delay, one frame image lag, '
      'a %.0f %% velocity-estimate error and a %.0f ms `stop()` coast.  All candidates use the '
      'same `Cam`/`Ctl` API and the same `move`/`measure`/`begin`/`next_sample`/`stop` calls '
      'as the firmware.' % (100 * meta['P'].noise, meta['P'].R, meta['P'].frame,
                            meta['P'].sweep_v, meta['P'].start_delay,
                            100 * meta['P'].vel_err, meta['P'].stop_lat))
    w('')
    w('**What the recovered real logs add to the model.**  The run parses the whole session '
      'pool (card3..card18) and uses it in three ways:')
    w('')
    sw = meta.get('sweep') or {}
    if sw.get('ok'):
        sig = sw['sig']
        st = sw.get('step_sig') or []
        w('- **AFSWEEP.LOG** (sweep geometry): %d file(s), %d unique series, %d fitted `s` '
          'curves (sigma median %.0f pulses, range %.0f-%.0f) against %d stepped same-direction '
          'fits (sigma median %.0f).  `af_replay` puts the accepted AFSWEEP series into the '
          'curve pool automatically (`sweep_series`, positions from time with the measured '
          'done time).  The wider sweep sigmas are kept: they are what makes the far-class '
          'results in this report different from the 22-curve pool of the earlier REPORT.md.'
          % (sw['files'], sw['series'], sw['ok'], base.med(sig), min(sig), max(sig),
             len(st), base.med(st) if st else float('nan')))
    else:
        w('- **AFSWEEP.LOG**: none found; sweep geometry stays an assumption.')
    if meta.get('afstop'):
        rows = []
        for f in meta['afstop']:
            rows += ['k%d plan=%d ms -> t_stop=%s t_done=%s' % (
                k, plan, st[0] if st else '-', st[1] if st else '-')
                for k, plan, st in f['rows']]
        w('- **AFSTOP.LOG** (stop test): ' + '; '.join(rows) + '.  Every planned stop still '
          'ended with FOCUS_DONE at the ~500 ms timer value and the samples continue past the '
          'planned stop time, i.e. there is **no evidence in these logs that `(9,3,1)` halted '
          'the `(9,1)` move**.  That is why the "stop does nothing" row is in the sensitivity '
          'tables and why C/G are treated as conditional.')
    if meta.get('afrtest', {}).get('files'):
        seqs = [' -> '.join(str(x[1]) for x in f['refs'])
                for f in meta['afrtest']['files']]
        w('- **AFRTEST.LOG** (Auto-refocus reference erosion): the two `RT_REF` sequences are '
          '%s `ref_s`; the per-re-landing ratios are %s (median **%.3f**).  The '
          'local-refocus experiment therefore uses that measured factor as the calibrated '
          'ratchet scenario, plus 0.85 as a worst case.'
          % ('; '.join(seqs),
             ', '.join('%.3f' % r for r in meta['afrtest']['ratios']),
             meta['afrtest']['ratchet']))
    w('')
    w('**New candidates (all on top of the same API).**')
    w('')
    for k, d, f in ALGOS:
        w('- **%s** - %s' % (k, d))
    w('')
    for k, d, f in LOCAL_ALGOS:
        w('- **%s** - %s' % (k, d))
    w('')
    w('More detail: **G** fits a parabola to `ln s` through the last 5 frame samples, '
      'predicts the vertex and calls `stop()` when the vertex is inside the modelled coast '
      'window `v_alg*(stop_lat + 0.5*frame)`; a 2-frame negative-slope / CUSUM rule is the '
      'fallback, and the proven stepped feedback retrace does the landing.  **H** climbs in '
      '40-pulse steps, fits the Gaussian with the floor free, and only jumps when R^2 >= 0.90, '
      'A/B >= 0.2, sigma in 8..110 and the peak is prominent; one verification sample decides '
      'between the jump and a Brent fallback.  **I** uses the same coarse bracket and then '
      'golden-section (6 iterations, pair samples).  **J** measures s at -40/0/+40 and uses '
      'the sign of the slope for the direction; a peak inside the span goes straight to '
      'Brent, a one-sided slope to the coarse climb.  **K** measures s at 0 and +60, inverts '
      'the *calibrated* Gaussian shape (`p* = D/2 + sigma_cal^2 ln(r)/D`, sigma_cal has a '
      '+-10 % error, floor +-10 %) and jumps; one verification, then the mirrored side '
      '(the documented side ambiguity), then B.')
    w('')
    w('**Metrics.**  Success = final true s >= 90 %% of the true peak s (noise-free, at the '
      'final true lens position).  "soft" = final true s in 75..90 %% (a near miss / a soft '
      'landing), "hard" = below 75 %%.  Time = simulated ms until the algorithm ends (capped '
      'at 15 s); moves = FOCUS_DONE count; travel = sum of |pulses|.  All algorithms see the '
      'same curves, start positions and per-trial seeds.  With N = %d per cell the 1-sigma '
      'Monte-Carlo error of a success rate is about %.1f points.'
      % (args.trials, 100.0 * math.sqrt(0.25 / max(args.trials, 1))))
    w('')
    w('## 2. Cold / warm start on the real curves (primary)')
    w('')
    w('Cells are `success / median time (s) / p90 time (s)`.  Cold = near + mid + far pooled.  '
      'Success = final true s >= 90 % of the true peak; "soft" = 75-90 %.')
    w('')
    rows = []
    for k, d, f in ALGOS:
        row = [k]
        for cls in ('near', 'mid', 'far', 'warm'):
            row.append(fmt_cell(main[(k, cls)]))
        row.append(fmt_cell(cold[k]))
        rows.append(row)
    w(base.md_table(['algo', 'near (15-120)', 'mid (120-300)', 'far (300-600)',
                     'warm (0-50)', 'cold'], rows))
    w('')
    w('**Cold pooled, all metrics.**')
    w('')
    rows = []
    for k, d, f in ALGOS:
        c = cold[k]
        rows.append([k, '%.0f%%' % c['succ'], '%.0f%%' % c['soft'], '%.1f' % c['t50'],
                     '%.1f' % c['t90'], '%.0f' % c['moves'], '%.0f' % c['travel'],
                     '%.1f%%' % c['to']])
    w(base.md_table(['algo', 'success', 'soft', 'median s', 'p90 s', 'moves',
                     'travel (pulses)', 'timeouts'], rows))
    w('')
    w('**Warm start, all metrics** (|x0| <= 50 pulses; this is the class where a local '
      'search is allowed to shortcut).')
    w('')
    rows = []
    for k, d, f in ALGOS:
        c = main[(k, 'warm')]
        rows.append([k, '%.0f%%' % c['succ'], '%.0f%%' % c['soft'], '%.1f' % c['t50'],
                     '%.0f' % c['moves'], '%.0f' % c['travel']])
    w(base.md_table(['algo', 'success', 'soft', 'median s', 'moves', 'travel'], rows))
    w('')
    if main_syn is not None:
        w('### 2b. The same algorithms on the synthetic curve family (secondary)')
        w('')
        w('Synthetic curves: `sigma_s` log-uniform 12-60 pulses, A/B 0.4-3, `g` width and '
          'amplitude tied to `s` (`af_replay.synth_curve`).  Same starts, seeds, noise and '
          'timing.  This is *not* field data; it widens the scene variety beyond the 48 real '
          'fits.')
        w('')
        rows = []
        for k, d, f in ALGOS:
            cr, cs = cold[k], cold_syn[k]
            rows.append([k, '%.0f%% / %.1f' % (cr['succ'], cr['t50']),
                         '%.0f%% / %.1f' % (cs['succ'], cs['t50']),
                         '%.0f%% / %.1f' % (cs['soft'], cs['t90'])])
        w(base.md_table(['algo', 'real curves: success / median s',
                         'synthetic: success / median s', 'synthetic: soft / p90 s'], rows))
        w('')
        tr = sorted(ALGOS, key=lambda x: -cold[x[0]]['succ'])[:3]
        ts = sorted(ALGOS, key=lambda x: -cold_syn[x[0]]['succ'])[:3]
        both = [a[0] for a in tr if a[0] in [b[0] for b in ts]]
        w('Top 3 on real curves: %s.  Top 3 on synthetic curves: %s.  Structures in both '
          'top-3 lists: **%s** - these are the results that do not depend on the curve family.'
          % (', '.join(a[0] for a in tr), ', '.join(a[0] for a in ts),
             ', '.join(both) if both else 'none'))
        w('')
    w('## 3. Sensitivity')
    w('')
    w('**Reversal offset R** (near + mid pooled, `success / median s / p90 s`).  R is not '
      'measured; the candidates keep the constants designed for R ~ 60.')
    w('')
    rows = [[k] + [fmt_cell(sens['R'][(k, r)]) for r in sens['R_list']] for k, d, f in ALGOS]
    w(base.md_table(['algo'] + ['R = %g' % r for r in sens['R_list']], rows))
    w('')
    w('**Reading noise** (near + mid; 1x = fitted ~1 %, 3x = high noise).')
    w('')
    rows = [[k] + [fmt_cell(sens['noise'][(k, nm)]) for nm in sens['noise_list']]
            for k, d, f in ALGOS]
    w(base.md_table(['algo', 'noise 1x', 'noise 3x'], rows))
    w('')
    w('**Stop latency** (near + mid; algorithms that call `stop()`; G brakes on an assumed '
      '60 ms coast, so a wrong latency is a model mismatch, as it is for the firmware).')
    w('')
    rows = [[k] + [fmt_cell(sens['lat'][(k, l)]) for l in sens['lat_list']]
            for k in ('B', 'C', 'G')]
    w(base.md_table(['algo'] + ['stop %g ms' % l for l in sens['lat_list']], rows))
    w('')
    w('**If stop() does nothing at all** (the field fact: `(9,3,1)` does not halt a running '
      '`(9,1)` move; we model it by letting the lens complete the whole command and telling '
      'the algorithm it travelled the full distance).  Near + mid; `success / median s / '
      'p90 s`.  B and G keep their accuracy (the retrace still starts from a consistent '
      'position) but lose the early-stop time; C, whose whole landing is stop-on-threshold, '
      'degrades most.')
    w('')
    rows = []
    for k in ('B', 'C', 'G'):
        cw = sens['noise'][(k, 1.0)]
        cd = sens['dead'][k]
        rows.append([k, '%.0f%% / %.1f / %.1f' % (cw['succ'], cw['t50'], cw['t90']),
                     '%.0f%% / %.1f / %.1f' % (cd['succ'], cd['t50'], cd['t90']),
                     '%.0f' % cd['moves']])
    w(base.md_table(['algo', 'stop works (60 ms)', 'stop does nothing', 'moves (dead)'], rows))
    w('')
    w('## 4. Reference erosion and the local Refocus search')
    w('')
    w('`afd_run_local` probes +-2*fine, climbs on the better side and otherwise takes an '
      '"in focus" exit.  In the field the Auto-refocus reference is re-set from the new '
      'settled sample after every landing (`aref_mpu_finish`/`aref_mpu_settle`), so it '
      'ratchets down toward the soft landings.  The recovered `AFRTEST.LOG` gives the real '
      'sequences:')
    w('')
    if meta.get('afrtest', {}).get('files'):
        rows = []
        for f in meta['afrtest']['files']:
            short = os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(f['path']))))
            rows.append([short,
                         ' -> '.join(str(x[1]) for x in f['refs']),
                         ', '.join('%.3f' % r for r in f['ratios']),
                         ', '.join('%d%s%s' % (p, 'ok' if o else 'X',
                                               '' if r else ' no-run')
                                   for p, o, r in f['trials'])])
        w(base.md_table(['session', 'ref_s sequence', 'per-re-land ratio', 'trial final_pct'],
                        rows))
        w('')
    w('We model the reference as an external *scenario* parameter (from the bookkeeping, not '
      'from the search): **fixed** at the true scene peak, **ratchet_field** = the median '
      'measured factor **%.3f** per landing, and **ratchet85** = 0.85 (a worst case).'
      % meta.get('ratchet', 0.866))
    w('')
    w('A sequence is 5 triggers of a tracked scene that drifts by +-(0-50 warm / 50-150 mid) '
      'pulses between triggers; the lens stays where the previous run landed.  Curves are '
      'shared by all variants and scenarios.  Only attempts with a trigger reading < 98 % of '
      'the reference are counted.  The real Refocus test scored no-trigger trials too (def=150 '
      'and def=400 never ran yet were scored), because the reference had been re-based '
      'downward; that is a test-design bug, not a local-search behaviour, and it is excluded '
      'here.')
    w('')
    for cls, lab in (('warm', 'warm/tracked, |defocus| 0-50'), ('mid', 'mid, |defocus| 50-150')):
        w('**%s.**  `triggered n / success / soft / lands 80-90%% of the pre-trigger '
          'reference / median final s (%% of peak) / moves / time (s) / fallback-to-B %% / '
          'reference drift (final/initial)`' % lab)
        w('')
        rows = []
        for variant, desc, fn in LOCAL_ALGOS:
            for mode in ('fixed', 'ratchet_field', 'ratchet85'):
                c = loc[(variant, cls, mode)]
                rows.append([variant, mode, '%d' % c['n'], '%.0f%%' % c['succ'],
                             '%.0f%%' % c['soft'], '%.0f%%' % c['ref80_90'],
                             '%.0f%%' % (100 * c['q50']),
                             '%.1f / %.1f' % (c['moves'], c['t50']),
                             '%.0f%%' % c['fb'], '%.2f' % c['ref_ratio']])
        w(base.md_table(['algo', 'reference', 'n', 'success', 'soft', 'lands 80-90%',
                         'median q', 'moves / median s', 'to B', 'ref final/init'], rows))
        w('')
    w('**Reference trajectory** (median reference / initial reference, per attempt index, '
      'warm class) - the ratchet scenario reproduces the 115k -> 77k erosion:')
    w('')
    rows = []
    for variant, desc, fn in LOCAL_ALGOS:
        for mode in ('fixed', 'ratchet_field', 'ratchet85'):
            tr = loc[(variant, 'warm', mode)]['ref_traj']
            rows.append([variant, mode] + ['%.2f' % v for v in tr])
    w(base.md_table(['algo', 'reference'] + ['a%d' % i for i in range(len(rows[0]) - 2)],
                    rows))
    w('')
    w('**Trigger split, warm class** (drop-path = trigger reading < 95 % of the reference; '
      'mild = 95-98 %):')
    w('')
    rows = []
    for variant, desc, fn in LOCAL_ALGOS:
        c = loc[(variant, 'warm', 'ratchet_field')]
        rows.append([variant, '%d' % c['drop_n'], '%.0f%%' % c['drop_succ'],
                     '%.0f%%' % c['drop_soft'], '%d' % c['mild_n'],
                     '%.0f%%' % c['mild_succ'], '%.0f%%' % c['mild_soft']])
    w(base.md_table(['algo', 'drop n', 'drop success', 'drop soft', 'mild n',
                     'mild success', 'mild soft'], rows))
    w('')
    if meta.get('afrtest', {}).get('files'):
        lv0 = loc[('local_v0', 'warm', 'ratchet_field')]
        lv1 = loc[('local_v1', 'warm', 'ratchet_field')]
        lv2 = loc[('local_v2', 'warm', 'ratchet_field')]
        real_trials = [t for f in meta['afrtest']['files'] for t in f['trials']]
        ran = [t for t in real_trials if t[2]]
        nok = sum(o for p, o, r in ran)
        w('**Field comparison.**  The two recovered Refocus tests scored %d/%d ok among the '
          '%d trials that actually ran (%d no-trigger trials were also scored because the '
          'reference had been re-based downward, a test-design bug); final_pct %s.  The '
          'simulated local_v0 leaves %.0f %% success and %.0f %% in the 80-90 %% band of the '
          'pre-trigger reference; local_v1/2 move that band down to %.0f %% / %.0f %%.  This '
          'is the same "accepts a slightly soft picture" signature as the log line '
          '`# local s0=82352 plus=36539 minus=86291 dir=0` followed by `# local in focus`: the '
          'minus probe is 104.8 %% of s0, just under the 105 %% IMPROVE threshold, so the '
          'current code takes the in-focus exit.  The mirror in this lab uses the same 105 %% '
          'rule, so v0 reproduces the case; v1 probes wider and falls back to B unless the '
          'trigger is close to the (high-water-mark) reference.'
          % (nok, len(ran), len(ran), len(real_trials) - len(ran),
             ', '.join('%d%s%s' % (p, 'ok' if o else 'X', '' if r else ' no-run')
                       for p, o, r in real_trials),
             lv0['succ'], lv0['ref80_90'], lv1['ref80_90'], lv2['ref80_90']))
        w('')
    w('**Reversal-offset sensitivity of the local search** (warm class, ratchet_field; the +-2*fine '
      'probe is only %.0f pulses and the reversal offset R is uniform in +-R, so at R = 60 the '
      'probe displacement itself is inside the reversal uncertainty).'
      % 40)
    w('')
    rows = []
    for variant, desc, fn in LOCAL_ALGOS:
        if variant == 'keep':
            continue
        rows.append([variant] + ['%.0f%% / %.1f s' % (loc[('R', variant, r)]['succ'],
                                                      loc[('R', variant, r)]['t50'])
                                 for r in loc['R_list']])
    w(base.md_table(['algo'] + ['R = %g' % r for r in loc['R_list']], rows))
    w('')
    w(local_reading(loc))
    w('')
    w('## 5. Ranking and recommendation')
    w('')
    rank = sorted(ALGOS, key=lambda x: (-cold[x[0]]['succ'], cold[x[0]]['t50']))
    rows = []
    for i, (k, d, f) in enumerate(rank, 1):
        c = cold[k]
        rows.append([i, k, '%.0f%%' % c['succ'], '%.1f' % c['t50'], '%.1f' % c['t90'],
                     '%.0f' % c['moves'], '%.0f' % c['travel'], '%.0f%%' % c['to']])
    w('**Ranked by cold success, then median time.**')
    w('')
    w(base.md_table(['#', 'algo', 'cold success', 'median s', 'p90 s', 'moves',
                     'travel', 'timeouts'], rows))
    w('')
    w(meta['recommendation'])
    w('')
    w('## 6. Assumptions and their uncertainty')
    w('')
    w('This tool re-uses the `af_replay.py` model and its documented uncertainties, and adds '
      'the following assumptions of its own:')
    w('')
    w('1. **Reversal offset R** (uniform, redrawn at each reversal, default 60 pulses, '
      'same-direction moves exact).  Unmeasured; the R table is the bound.  All candidates '
      'were designed with R ~ 60 in mind, none was retuned after the fact.')
    w('2. **Sweep speed 0.40 pulses/ms** is still a parameter: the AFSWEEP logs give geometry '
      'and the fitted widths, but their `done` times are also the ~510 ms timer in several '
      'cases, so a true time->position calibration stays open.  G, H/I/J/K fallbacks and the '
      'step-count budgets all depend on it.')
    w('3. **Stop latency** (default 60 ms) is unmeasured.  G brakes using an assumed 60 ms '
      'coast; with a true 200 ms it overshoots.  The latency table is the bound and the first '
      'field test (stop a running (9,1) move at several delays) would fix it.')
    w('4. **Curve pool**: %d real fitted curves, including the accepted AFSWEEP series; the '
      'synthetic family (section 2b) is the widened-scene secondary check.  A single Gaussian '
      'on a flat floor; the real tails, false peaks and texture changes are not modelled.  The '
      'pool is larger and broader than the 22-curve pool of the earlier REPORT.md because the '
      'recovered card3..card18 logs add fits.' % meta['pool'])
    w('5. **Reading noise** is multiplicative Gaussian, ~1 % per reading (0.98 % fitted '
      'from these logs), independent; real outliers, exposure steps and motion blur are not '
      'modelled except in the erosion scenario.')
    w('6. **Reference bookkeeping** in the local experiment is a scenario: fixed, the measured '
      'median ratchet **%.3f** (per re-landing ratios from the two AFRTEST.LOG files), and a '
      '0.85 worst case.  The measured ratios span 0.82-0.91 and are not a single constant; the '
      'scenario bracket reflects that.  The scene drift between triggers (0-50 / 50-150 '
      'pulses) and the 5-trigger sequence are assumptions; the end stop (the suspected cause '
      'of the real +60/+150 failures) is *not* modelled, so the local numbers probably '
      'understate the field failure rate.  The real test also scored no-trigger trials (a '
      'test-design bug caused by the reference re-basing); those are excluded here.'
      % meta.get('ratchet', 0.866))
    w('7. **DFD (K)** assumes a calibrated `sigma_s(p)` (+-10 %) and a floor estimate '
      '(+-10 %); the real calibration table does not exist yet.  It also uses s rather than a '
      'spectral relative-blur estimate, so K is an optimistic stand-in for DFD.')
    w('8. **stop()** is assumed to work in the main runs (coast = latency x speed).  The '
      '"stop does nothing" row models the contrary field observation; whether any command '
      'halts a (9,1) move is untested, and it changes the value of C/G more than any threshold.')
    w('9. **Algorithm constants** of the new candidates were set once by reasoning, not '
      'optimised; the tables rank structures, not the best constants.  The Monte-Carlo error '
      'at N = %d is ~%.1f points per cell; with two curve families (real primary, synthetic '
      'secondary) the ranking claims should be read as the intersection of the two.'
      % (args.trials, 100.0 * math.sqrt(0.25 / max(args.trials, 1))))
    w('')
    w('Reproduce with:')
    w('')
    w('```')
    w('python3 contrib/af_replay/af_lab.py --trials %d --seed %s \\' % (args.trials, args.seed))
    w('    --logs %s --report contrib/af_replay/REPORT_LAB.md' % meta['logs'])
    w('```')
    w('')
    return '\n'.join(L)


def local_reading(loc):
    v0 = loc[('local_v0', 'warm', 'ratchet_field')]
    v1 = loc[('local_v1', 'warm', 'ratchet_field')]
    v2 = loc[('local_v2', 'warm', 'ratchet_field')]
    B = loc[('B', 'warm', 'ratchet_field')]
    v0f = loc[('local_v0', 'warm', 'fixed')]
    m0 = loc[('local_v0', 'mid', 'ratchet_field')]
    m1 = loc[('local_v1', 'mid', 'ratchet_field')]
    m2 = loc[('local_v2', 'mid', 'ratchet_field')]
    r0 = loc[('R', 'local_v0', 0)]
    r6 = loc[('R', 'local_v0', 60)]
    r10 = loc[('R', 'local_v0', 100)]
    rb0 = loc[('R', 'B', 0)]
    rb6 = loc[('R', 'B', 60)]
    return (
        'Reading: variant 0 is the current code.  In the warm class with a **fixed** '
        'reference it already succeeds only %.0f %% and lands at 80-90 %% of the reference in '
        '%.0f %% of the triggered attempts: the +-2*fine probes are blind at roughly half the '
        'probe span, so the "no side improves" exit is taken at a genuinely defocused '
        'position.  Under the measured ratchet its acceptance threshold falls with the '
        'reference (trajectory 1.00 -> %.2f), success is %.0f %% and the 80-90 %% band is '
        '%.0f %%.  Variant 1 (wider probe, no in-focus exit on a drop trigger) reaches %.0f %% '
        '(soft %.0f %%, 80-90 %% band %.0f %%); variant 2 (per-scene high-water-mark '
        'reference) keeps the effective reference at 1.00 instead of %.2f, reaches %.0f %% '
        'and cuts the 80-90 %% band to %.0f %%, at the cost of more B fallbacks (%.0f %% vs '
        '%.0f %%).  For the mid class the difference is smaller (%.0f %% / %.0f %% / %.0f %%) '
        'because most triggers already fall back to B.  Even the fixed local search stays '
        'below the full-sweep B on the same triggers (%.0f %% vs %.0f %%), and B is not slower '
        'here, so the local shortcut buys moves, not accuracy.  The reversal-offset table is '
        'the reason: at **R = 0** local_v0 reaches %.0f %% (B %.0f %%), at **R = 60** it falls '
        'to %.0f %% (B %.0f %%) and at **R = 100** to %.0f %%: a 40-pulse probe is inside the '
        'reversal uncertainty, so the direction decision is close to a coin flip.  The '
        'acceptance-rule fix is cheap and worth porting, but the local search only pays off '
        'once the reversal offset is measured or compensated.'
        % (v0f['succ'], v0f['ref80_90'], v0['ref_ratio'], v0['succ'], v0['ref80_90'],
           v1['succ'], v1['soft'], v1['ref80_90'], v1['ref_ratio'], v2['succ'],
           v2['ref80_90'], v2['fb'], v1['fb'], m0['succ'], m1['succ'], m2['succ'],
           B['succ'], v0f['succ'], r0['succ'], rb0['succ'], r6['succ'], rb6['succ'],
           r10['succ']))


def recommendation_text(cold, sens, loc, cold_syn=None):
    rank = sorted(cold, key=lambda k: (-cold[k]['succ'], cold[k]['t50']))
    b, c, g = cold['B'], cold['C'], cold['G']
    f, h, i_, j, kk = cold['F'], cold['H'], cold['I'], cold['J'], cold['K']
    dead = sens['dead']
    v0 = loc[('local_v0', 'warm', 'ratchet_field')]
    v1 = loc[('local_v1', 'warm', 'ratchet_field')]
    v2 = loc[('local_v2', 'warm', 'ratchet_field')]
    out = []
    out.append('**Headline.**  Cold (near+mid+far), real curves, N per cell: ' + '; '.join(
        '**%s** %.0f %% / %.1f s' % (k, cold[k]['succ'], cold[k]['t50']) for k in rank) + '.')
    out.append('')
    if cold_syn is not None:
        tr = sorted(cold, key=lambda k: -cold[k]['succ'])[:3]
        ts = sorted(cold_syn, key=lambda k: -cold_syn[k]['succ'])[:3]
        both = [a for a in [x[0] for x in tr] if a in [x[0] for x in ts]]
        out.append('**Curve-family check.**  Top 3 on the real curves: %s; top 3 on the '
                   'synthetic family: %s; in both: **%s**.  A structure that is near the top '
                   'on both curve families is the one to trust; a structure that only wins on '
                   'one family (or on a threshold constant) should not drive the port.'
                   % (', '.join(x[0] for x in tr), ', '.join(x[0] for x in ts),
                      ', '.join(both) if both else 'none'))
        out.append('')
    order = ' > '.join(k for k in rank)
    out.append('1. **The simulated ranking is %s; the two structures that depend on stop() '
               'are C and G.**  C %.0f %% / %.1f s, G %.0f %% / %.1f s, B %.0f %% / %.1f s '
               '(the baseline).  C lands by stop-on-threshold and has no other landing; when '
               'the field fact that `(9,3,1)` does not halt a running `(9,1)` move is '
               'modelled, C falls to %.0f %% (B stays %.0f %%, G %.0f %%).  Predictive '
               'braking (G) is a stop decision too: with a dead stop it cannot brake and '
               'degrades to a full sweep, keeping B-like accuracy but losing the time it was '
               'meant to save.  The shippable conclusion today is to keep B (it is robust to a '
               'dead stop and needs no new command) and to invest in the landing/acceptance '
               'rule rather than in a new search; do not build a new search around stop() '
               'before the stop test exists.  F is only a small win over the stepped A and '
               'nowhere near B, so it is not the priority here.'
               % (order, c['succ'], c['t50'], g['succ'], g['t50'], b['succ'], b['t50'],
                  dead['C']['succ'], dead['B']['succ'], dead['G']['succ']))
    out.append('2. **The single measurement that most changes the roadmap is whether any '
               'command halts a running move** (then C/G/predictive braking become available; '
               'the model puts the best stop-dependent structure up to ~%.0f points above B '
               'and shortens the p90 tail), followed by the reversal-offset distribution.  If '
               'the stop works, predictive braking (G) is the target with C as the alternative; '
               'if it does not, the sweep must run to its end and the win is only in the '
               'landing rule.'
               % max(0.0, max(c['succ'], g['succ']) - b['succ']))
    out.append('3. **Auto refocus: port the handover 4.1/4.2 fixes, but the reversal offset is '
               'the real blocker.**  On the measured ratchet the current logic has %.0f %% '
               'success and leaves %.0f %% in the 80-90 %% band of the pre-trigger reference; '
               'the wider probe plus "no in-focus exit on a drop trigger" raises success to '
               '%.0f %% (band %.0f %%), and adding the per-scene high-water mark keeps the '
               'effective reference at 1.00 with %.0f %% success and a %.0f %% band.  Both are '
               'cheap and need no new command, so port local_v2 behind the existing trigger.  '
               'But the model says the local search only approaches B when R is small; '
               'measuring/compensating R is worth more than the probe change.  Also consider '
               'running B directly for the Refocus test: in this model it is both more '
               'accurate and not slower for small defocus.'
               % (v0['succ'], v0['ref80_90'], v1['succ'], v1['ref80_90'],
                  v2['succ'], v2['ref80_90']))
    out.append('4. **Model-fit + jump (H) and Brent (I) are not competitive as cold-start '
               'engines.**  H %.0f %% / %.1f s, I %.0f %% / %.1f s vs B %.0f %% / %.1f s.  The '
               'reason is structural: a 40-pulse staircase cannot resolve a 10-20-pulse peak, '
               'and every absolute jump/probe adds a reversal whose offset the algorithm does '
               'not know (the R table: H/I fall from ~90 %% at R = 0 to ~45 %% at R = 150).  '
               'Keep the idea only as a warm-start "well-behaved curve, jump" branch, with a '
               'sweep fallback.'
               % (h['succ'], h['t50'], i_['succ'], i_['t50'], b['succ'], b['t50']))
    out.append('5. **Two-sided slope probe (J) is cheap and close to B** (%.0f %% / %.1f s '
               'cold).  It is a low-risk direction replacement for the fixed probe, but it '
               'does not change the overall structure.  **DFD (K)** (%.0f %% / %.1f s) cannot '
               'be recommended: besides the missing `sigma_s(p)` calibration it fails exactly '
               'where DFD is needed (low contrast, far defocus) and the floor/side ambiguity is '
               'real, as the implementation doc warned.'
               % (j['succ'], j['t50'], kk['succ'], kk['t50']))
    out.append('6. **Field tests that would confirm/refute this, in order:**  (i) a stop() '
               'test on a running `(9,1)` move at several delays (does it halt? how far does '
               'it coast?) - decides C/G; (ii) a reversal test with many repeats at several '
               'move sizes - the distribution of R decides the local search and every jump '
               'algorithm; (iii) AFSWEEP.LOG at several sweep speeds - fixes the '
               'time->position model B/G depend on; (iv) a Refocus test with local_v2 and '
               'logging of the scene high-water mark, the landing s and the trigger reading - '
               'confirms the ratchet and whether the HWM stops it.  The signal to watch in '
               '(iv): if the logged reference still falls after a landing, the '
               'aref_mpu_finish bookkeeping is the bug, not the search.')
    out.append('')
    out.append('(Note: the baselines are re-run inside this harness, so their numbers differ '
               'from REPORT.md by Monte-Carlo noise and by the larger pool of recovered logs; '
               'the comparison between algorithms is apples-to-apples because they share '
               'curves, starts and seeds.)')
    return '\n'.join(out)


# ========================================================================================
# CLI
# ========================================================================================

def find_logs(explicit):
    if explicit:
        return explicit
    env = os.environ.get('AF_REPLAY_LOGS')
    if env and os.path.isdir(env):
        return env
    cands = [os.path.join(HERE, 'logs'), os.path.join(HERE, '..', '..', 'logs')]
    cands += sorted(glob.glob('/private/tmp/claude-501/*Crop-mood-eosm-slim-gui/*/scratchpad'))
    for c in reversed(cands):
        if os.path.isdir(c) and (glob.glob(os.path.join(c, '**', 'AFDRIVE.LOG'), recursive=True)
                                 or glob.glob(os.path.join(c, '**', 'AFCALIB.LOG'), recursive=True)):
            return c
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description='Candidate AF algorithm lab on top of af_replay')
    ap.add_argument('--logs', default=None,
                    help='folder with AFDRIVE/AFCALIB/AFSWEEP logs (searched recursively); '
                         'default: $AF_REPLAY_LOGS, contrib/af_replay/logs, or the known '
                         'scratchpad')
    ap.add_argument('--trials', type=int, default=200, help='trials per defocus class (200)')
    ap.add_argument('--seed', default='lab1')
    ap.add_argument('--report', default=os.path.join(HERE, 'REPORT_LAB.md'))
    args = ap.parse_args(argv)

    t_start = time.time()
    logs = find_logs(args.logs)
    P = base.Params()
    pool = []
    data = fits = None
    sweep = dict(files=0, series=0, ok=0, sig=[], step_sig=[])
    afstop, afrtest = [], dict(files=[], ratios=[], ratchet=0.866)
    if logs:
        data = base.load_logs(logs)
        fits = base.fit_all_series(data)
        pool = base.curve_pool(fits)
        tm = base.solve_move_time(data['runs'])
        ov = base.solve_overheads(data['runs'])
        noise = base.solve_noise(data['runs'], data['blocks'])
        P = base.calibrate(base.Params(), tm, ov, noise)
        sweep = afsweep_summary(data, fits)
        afstop = parse_afstop(logs)
        afrtest = parse_afrtest(logs)
        print('logs: %s -> %d runs, %d fitted series, curve pool %d, noise %.2f %%, '
              'move time %.0f+%.2f|n|' % (logs, len(data['runs']),
                                          len([x for x in fits if x['kind'] != 'MPUDRIVE']),
                                          len(pool), 100 * P.noise, P.a_fast, P.b_fast))
        print('  AFSWEEP: %d file(s), %d unique series, %d fitted; AFSTOP: %d file(s); '
              'AFRTEST: %d file(s), ratchet median %.3f'
              % (sweep['files'], sweep['series'], sweep['ok'], len(afstop),
                 len(afrtest['files']), afrtest['ratchet']))
    else:
        print('no logs found: using synthetic curves (pass --logs to use the real fits)')
    real_curves = bool(pool)
    if not pool:
        pool = [base.synth_curve(random.Random('%s/synth/%d' % (args.seed, i)))
                for i in range(40)]

    main_res, cold = run_main(P, pool, args.trials, args.seed,
                              'real' if real_curves else 'synth')
    # secondary curve family: the same algorithms on the synthetic curves
    main_syn, cold_syn = run_main(P, pool, args.trials, args.seed, 'synth')
    sens = run_sensitivity(P, pool, args.trials, args.seed)
    ratchet = afrtest['ratchet'] if afrtest['files'] else 0.866
    loc = run_local(P, pool, max(150, args.trials), args.seed, ratchet)
    meta = dict(P=P, pool=len(pool), logs=logs or '(synthetic curves)',
                real_curves=real_curves, sweep=sweep, afstop=afstop, afrtest=afrtest,
                ratchet=ratchet, data=data, elapsed=time.time() - t_start)
    meta['recommendation'] = recommendation_text(cold, sens, loc, cold_syn)
    rep = build_report(args, meta, main_res, cold, sens, loc, main_syn, cold_syn)
    with open(args.report, 'w') as f:
        f.write(rep + '\n')
    meta['elapsed'] = time.time() - t_start
    # regenerate with the final elapsed time in the header
    rep = build_report(args, meta, main_res, cold, sens, loc, main_syn, cold_syn)
    with open(args.report, 'w') as f:
        f.write(rep + '\n')
    print('wrote %s (%.1f s)' % (args.report, meta['elapsed']))
    for k, d, f in ALGOS:
        c = cold[k]
        print('  %-2s cold: success %3.0f%%  soft %2.0f%%  median %4.1f s  moves %2.0f  '
              'travel %4.0f' % (k, c['succ'], c['soft'], c['t50'], c['moves'], c['travel']))
    print('  local (field-ratchet, warm): ' + '; '.join(
        '%s %.0f%% succ / %.1f s' % (v, loc[(v, 'warm', 'ratchet_field')]['succ'],
                                     loc[(v, 'warm', 'ratchet_field')]['t50'])
        for v, d, f in LOCAL_ALGOS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
