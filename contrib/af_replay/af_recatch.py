#!/usr/bin/env python3
"""af_recatch - offline evidence: AF patch 11 "re-catch" landing vs the current B ("classic") landing.

Stdlib only.  A time-domain lens simulator with imperfections (peak-estimate error, unmeasured-move gain/offset error,
backlash, stop-does-not-halt, measurement noise, end stops, per-move time costs) and two (three) controllers that mirror
modules/crop_rec.c (afd_run_sweep / afd_sweep_find / afd_sweep / afd_land_steps / afd_retrace) and
doc/AF-PATCH11-RECATCH.md (afd_land_recatch + bounded fallback + 1F coarse change).

  python3 af_recatch.py                      # full grid, 300 trials per cell, markdown table on stdout
  python3 af_recatch.py --trials 100 --lens 55 --err realistic --defocus 150
  python3 af_recatch.py --trace 55 realistic 150 3 classic     # one run, move log

Extra variants 'classic_top2' / 'recatch_top2' (not in the C code / spec): land_top tries both sides; 'classic_notop' / 'recatch_notop': the landing skips land_top.  Algorithms: 'classic' = the patch-10 field build; 'recatch' = spec; 'classic_fix' = classic + the spec's
"classic path fix" (retrace flat detection only after the walk covered 120 pulses).
Units: pulses (lens steps) and ms.  All s values are in one scale (the "ex" scale of the C code).
Model assumptions are listed in ASSUMPTIONS at the bottom (also printed by --assumptions).
"""
import argparse
import math
import random
import statistics
import sys
from dataclasses import dataclass

# ---- constants mirrored from crop_rec.c / the spec ---------------------------------------------------
SPD07, SPD1F = 7, 0x1F
CHUNK = {SPD07: 100, SPD1F: 300}
MSPP_X10 = {SPD07: 29, SPD1F: 10}
SW_LAT_MS, SW_TRACK_PAD_MS, SW_PRE, SW_MARGIN, SW_ERR = 120, 150, 60, 80, 100
SW_MAX_CHUNKS, SW_RESWEEP_CHUNKS, SW_COARSE_CHUNKS, SW_COARSE_BACK = 12, 4, 6, 300
SW_FALL_PCT, SW_FALL_N, SW_FALL_END_PCT, NOPEAK_PCT = 60, 2, 85, 115
RT_BACK2, RT_BACK, RT_THR1, RT_THR2, RT_SLACK, RT_EXTRA, RT_FALL_PCT = 140, 60, 90, 80, 4, 40, 80
FLAT_RT_N, FLAT_RT_PCT, FLAT_RT_REF_PCT = 4, 3, 60
ES_N, ES_MS, ENDSTOP_MIN, ENDSTOP_PCT, CAP_RANGE, CAP_G_PCT, ES_EARLY_CHUNKS = 2, 150, 40, 2, 1000, 130, 4
RUN_CAP_B_MS, MAX_TOTAL = 18000, 9000
PROBE_PCT, COARSE = 95, 100
RC_L1, RC_PASSES, RC_SEEN_PCT, RC_PASSED_PCT, RC_LOST_PCT, RC_MAX_CHUNKS = 50, 4, 50, 85, 30, 24
FLAT_MIN_TRAVEL_FIX = SW_MARGIN + 40


def sgn(x):
    return 1 if x > 0 else (-1 if x < 0 else 0)


def clip(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def below(a, b, pct):          # afd_below: a < b * pct / 100
    return a * 100 < b * pct


def close(a, b, pct):          # afd_close: |a-b| <= pct % of b
    return abs(a - b) * 100 <= b * pct


class Abort(Exception):
    def __init__(self, why):
        Exception.__init__(self, why)
        self.why = why


# ---- configuration -------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Lens:
    name: str
    sig: float            # s peak sigma (pulses)
    tail_w: float = 0.0   # weight of a broad pedestal on the peak
    tail_sig: float = 60.0
    floor: float = 16300.0
    peak: float = 125000.0
    sig_g: float = 100.0  # g (broad) sigma
    g_floor: float = 15500.0
    g_gain: float = 0.25  # g peak = floor * (1 + gain)
    fine: int = 10        # AFD_FINE (focal-length table)


LENSES = {
    '55': Lens('55mm narrow', 12.0, tail_w=0.0, fine=10),
    '18': Lens('18mm', 21.0, fine=20),
    '50w': Lens('EF50 wide', 45.0, sig_g=150.0, fine=10),
    '55t': Lens('55mm +pedestal', 12.0, tail_w=0.3, tail_sig=60.0, fine=10),   # sensitivity: sweep traces look wider than static walks
}


@dataclass(frozen=True)
class Err:
    name: str
    pe: float             # sweep peak-estimate error: uniform +-pe pulses (via start latency of the lens)
    o_gain: float         # fast (1F / unmeasured) move: relative gain error, uniform +-
    o_off: float          # ... offset error, uniform +- pulses per command chunk
    o_big_p: float        # ... probability of an extra large error per chunk
    o_big: tuple          # ... its magnitude range (sign random)
    f1_speed: float       # 1F tracked sweep: relative error of the true speed vs the model
    backlash: tuple       # per-run lost motion on a direction reversal, uniform (lo, hi) pulses
    step_sd: float        # relative sd of measured / 07 moves
    stop_halts: bool      # does (9,3,1) stop a tracked move?  (field: no)


ERRS = {
    'none': Err('none', 0, 0, 0, 0, (0, 0), 0, (0, 0), 0, True),
    'realistic': Err('realistic', 60, 0.08, 50, 0.15, (80, 250), 0.15, (0, 4), 0.02, False),
    'bad': Err('bad', 100, 0.12, 60, 0.25, (80, 250), 0.30, (2, 10), 0.04, False),
    # sensitivity: the failures come from the sweep's peak estimate (and the stop), not from the unmeasured moves
    'estbad': Err('estbad', 100, 0.03, 15, 0.03, (80, 250), 0.15, (0, 4), 0.02, False),
}

NOISE_REL = 0.025        # sd of s / g relative to (s - floor)
NOISE_FLOOR = 0.010      # sd relative to the floor level (field: floor readings scatter ~1 %)


# ---- simulated lens -----------------------------------------------------------------------------------
class Sim:
    def __init__(self, lens, err, rng, defocus, wrng, noise=NOISE_REL, log=False):
        self.lens, self.err, self.rng, self.noise = lens, err, rng, noise
        self.lo = -wrng.uniform(0.25, 0.75) * 2000.0     # true end stops relative to the peak (travel range 2000)
        self.hi = self.lo + 2000.0
        self.x = float(defocus * wrng.choice((-1, 1)))
        self.bl = wrng.uniform(*err.backlash)
        self.gpk = wrng.gauss(0, 15)
        self.x_start = self.x
        self.t = 0.0
        self.last_dir = 0
        self.travel = 0           # sum of commanded |pulses| (afd_travel)
        self.moves = 0            # afd_moves
        self.es_contacts = 0      # true end-stop contacts (command clipped by the stop)
        self.max_dev = 0.0
        self.log = [] if log else None
        # controller (afd_*) state
        self.pos = 0
        self.endstop_dir = 0
        self.es_dir = self.es_n = self.es_acc = 0
        self.base_ok = False
        self.base_s = self.base_g = 0.0
        self.last_g = 0.0
        self.land_flat = False
        self.flat_det = False
        self.flat_min_travel = 0
        self.resweep_used = False
        self.nopeak = False
        self.sw_max0 = 0.0
        self.samples = []
        self.tm = False           # patch-12 time model (field facts of 2026-10-19), set by run_trial for the *_l30 / *_tm variants
        self.frame = 42.0         # LV frame period (44 ms in the patch-12 time model)

    # -- curves
    def s_true(self, x):
        L = self.lens
        f = (1 - L.tail_w) * math.exp(-x * x / (2 * L.sig ** 2)) + L.tail_w * math.exp(-x * x / (2 * L.tail_sig ** 2))
        return L.floor + (L.peak - L.floor) * f

    def g_true(self, x):
        u = x - self.gpk
        return self.lens.g_floor * (1 + self.lens.g_gain * math.exp(-u * u / (2 * self.lens.sig_g ** 2)))

    def quality(self):
        return self.s_true(self.x) / self.lens.peak

    def _noisy(self, v, floor, k):
        sd = math.sqrt((NOISE_FLOOR * floor) ** 2 + (self.noise * (v - floor)) ** 2) / math.sqrt(k)
        return max(1.0, v + self.rng.gauss(0, sd))

    def read(self, x, k=1, blur=0.0):
        """one s / g reading with the lens at x; blur = half exposure travel in pulses (tracked moves)"""
        if blur:
            s = (self.s_true(x - blur) + self.s_true(x) + self.s_true(x + blur)) / 3
            g = (self.g_true(x - blur) + self.g_true(x) + self.g_true(x + blur)) / 3
        else:
            s, g = self.s_true(x), self.g_true(x)
        return self._noisy(s, self.lens.floor, k), self._noisy(g, self.lens.g_floor, k)

    def _lg(self, msg):
        if self.log is not None:
            self.log.append('%6d %s' % (self.t, msg))

    def _guard(self, n):
        if self.t > RUN_CAP_B_MS:
            raise Abort('timeout')
        if self.travel + abs(n) > MAX_TOTAL:
            raise Abort('travel')

    # -- physics of one command chunk
    def _phys(self, c, speed):
        """true signed travel of the command chunk c (pulses); returns (new_x - old_x, refused)"""
        e, r = self.err, self.rng
        d = sgn(c)
        if speed == SPD1F:
            tr = c * (1 + r.uniform(-e.o_gain, e.o_gain)) + r.uniform(-e.o_off, e.o_off)
            if e.o_big_p and r.random() < e.o_big_p:
                tr += r.choice((-1, 1)) * r.uniform(*e.o_big)
        else:
            tr = c * (1 + r.gauss(0, e.step_sd))
        if self.last_dir and d != self.last_dir:
            tr -= d * (self.bl + (r.gauss(0, 1.5) if e.backlash[1] else 0))
        tr = d * max(0.0, d * tr)
        nx = clip(self.x + tr, self.lo, self.hi)
        act = nx - self.x
        if abs(c) >= ENDSTOP_MIN and abs(act) < abs(tr) - 10:
            self.es_contacts += 1
        refused = abs(c) >= ENDSTOP_MIN and abs(act) < 0.1 * abs(c)
        if abs(act) > 0.5:
            self.last_dir = d
        self.x = nx
        self.max_dev = max(self.max_dev, abs(nx - self.x_start))
        return act, refused

    def _es_check(self, d, ln, pulses, refused):
        """afd_es_check"""
        if abs(ln) < ENDSTOP_MIN:
            return False
        if not refused:
            self.es_n = self.es_acc = 0
            return False
        if d != self.es_dir:
            self.es_dir, self.es_n, self.es_acc = d, 0, 0
        self.es_n += 1
        self.es_acc += pulses
        if self.es_n < ES_N:
            return False
        self.pos -= d * self.es_acc
        self.es_n = self.es_acc = 0
        self.endstop_dir = d
        self._lg('# endstop dir=%d how=flags pos=%d' % (d, self.pos))
        return True

    def _done_ms(self, c, rev):
        """patch-12 time model of a measured 07 move: FOCUS_DONE on completion (153 + 1.33 |p| ms); moves of 31..100 pulses hit the
        ~510 ms timer 50 % of the time (94 % when the move reverses direction; 1-3 % up to 30 pulses)"""
        a = abs(c)
        done = 153.0 + 1.33 * a
        pt = 0.02 if a <= 30 else (0.94 if rev else 0.5)
        return max(done, 510.0) if self.rng.random() < pt else done

    def settle_rest(self):
        """patch 11.1 / S4 rest measurement after a re-catch chunk: frame-synced, wait until 3 frames in a row agree within 5 %,
        at most 12 frames.  Model: the lens rings down after FOCUS_DONE, a residual offset U(+-8) pulses halving every frame.
        Returns (s, settled)."""
        self._guard(0)
        self.t += self.rng.uniform(0, self.frame)
        resid = self.rng.uniform(-8, 8)
        vals = []
        for k in range(12):
            self.t += self.frame
            s, g = self.read(self.x + resid * 0.5 ** k, 1)
            vals.append(s)
            if len(vals) >= 3 and close(max(vals[-3:]), min(vals[-3:]), 5):
                self.last_g, self.base_ok = g, False
                return sum(vals[-3:]) / 3.0, True
        self.last_g, self.base_ok = g, False
        return vals[-1], False

    # -- afd_move_ex
    def move_ex(self, phase, n, sample, track=0):
        """n pulses (0 = just sample); sample 0 none (fast unmeasured), 1 stable pair, 2 one sample.  Returns s (0 if none)."""
        self._guard(n)
        refused_any = False
        if n:
            speed = SPD07 if sample else SPD1F
            cap = CHUNK[speed]
            rem, nch, dtot = n, 0, 0.0
            while rem:
                c = clip(rem, -cap, cap)
                rev = bool(self.last_dir) and sgn(c) != self.last_dir
                act, refused = self._phys(c, speed)
                self.pos += c
                self.travel += abs(c)
                rem -= c
                nch += 1
                refused_any = refused_any or refused
                if sample:
                    if refused:
                        mv = 100.0
                    elif self.tm:
                        mv = self._done_ms(c, rev) + 42 + 70
                    else:
                        mv = max(SW_LAT_MS + 2.9 * abs(c), 510.0 if abs(c) >= 50 else 0.0) + 42 + 70
                else:
                    mv = 100.0 if refused else 250.0 + abs(c) + (150.0 if nch > 1 else 0.0) + (90.0 if rem == 0 else 0.0)
                self.t += mv
                if self._es_check(sgn(c), c, c, refused):
                    break
            self.moves += 1
        s = 0.0
        if sample:
            self.t += (110.0 if n == 0 else 0.0) + (60.0 if sample == 1 else 0.0)
            s, g = self.read(self.x, 2 if sample == 1 else 1)
            self.last_g = g
            if n and abs(n) >= ENDSTOP_MIN and self.base_ok and refused_any and close(s, self.base_s, ENDSTOP_PCT) \
                    and close(g, self.base_g, ENDSTOP_PCT):
                self.endstop_dir = sgn(n)
            self.base_s, self.base_g, self.base_ok = s, g, True
        elif n:
            self.base_ok = False
        self._lg('%s %d pos=%d s=%d x=%d' % (phase, n, self.pos, s, self.x))
        return s

    # -- afd_sweep: one tracked move
    def sweep(self, tag, d, ln, speed, cb, pad=SW_TRACK_PAD_MS):
        self._guard(ln)
        e, r = self.err, self.rng
        self.sw_seen = 0
        self.sw_max = self.sw_max0
        self.sw_below = 0
        self.sw_max0 = 0.0
        ln = min(ln, CHUNK[speed])
        mspp = MSPP_X10[speed]
        t_exp = SW_LAT_MS + ln * mspp / 10.0
        track_end = t_exp + pad
        x0 = self.x
        tr, refused = self._phys(d * ln, speed)
        ms_pp = mspp / 10.0
        vscale = 1 + (r.uniform(-e.f1_speed, e.f1_speed) if speed == SPD1F else 0.0)
        pe = e.pe * (1.5 if speed == SPD1F else 1.0)
        lat = SW_LAT_MS + r.uniform(-pe, pe) * ms_pp
        dur = abs(tr) * ms_pp * vscale
        if refused:
            track_end = 130.0
        ph = r.uniform(0, self.frame)
        samp = []                       # (t_ms, s, g)
        stop_ms = -1
        k = 0
        blur = 12.0 / (ms_pp * vscale)   # half an exposure (~12 ms) of travel
        while len(samp) < 96:
            tt = ph + self.frame * k
            k += 1
            if tt > track_end:
                break
            f = clip((tt - lat) / dur, 0.0, 1.0) if dur > 0 else 0.0
            s, g = self.read(x0 + tr * f, 1, 0.0 if refused else blur)
            samp.append((tt, s, g))
            if cb is not None and cb(self, len(samp), s):
                stop_ms = tt
                break
        n = len(samp)
        end_wall = max(track_end, lat + dur) + 40.0
        if stop_ms >= 0 and e.stop_halts:
            f = clip((stop_ms + 40 - lat) / dur, 0.0, 1.0) if dur > 0 else 0.0
            self.x = x0 + tr * f
            end_wall = stop_ms + 40.0 + 40.0
        self.t += 100.0 if refused else end_wall
        self.pos += d * ln
        self.travel += ln
        self.moves += 1
        self.base_ok = False
        moved = ln if stop_ms < 0 else int(clip((stop_ms + 40 - SW_LAT_MS) * 10 / mspp, 0, ln))
        self.pos -= d * (ln - moved)
        w = dict(n=n, samp=samp, endstop=False, stop_ms=stop_ms, moved=moved, peak=False, smax=0.0, smin=0.0, over=0,
                 pos_pk=0, gmax=0.0, gfirst=samp[0][2] if n else 0.0)
        s_list = [q[1] for q in samp]
        if n:
            smax, smin = max(s_list), min(s_list)
            imax = s_list.index(smax)
            w['gmax'] = max(q[2] for q in samp)
            gmn = min(q[2] for q in samp)
            if self._es_check(d, ln, moved, refused):
                w['endstop'] = True
            elif ln >= ENDSTOP_MIN and n >= 2 and close(smax, smin, ENDSTOP_PCT) and close(w['gmax'], gmn, ENDSTOP_PCT) and refused:
                w['endstop'] = True
                self.pos -= d * moved
                self.endstop_dir = d
            tpk = samp[imax][0]
            if 0 < imax < n - 1:
                sm, s0, sp = s_list[imax - 1], s_list[imax], s_list[imax + 1]
                den = sm - 2 * s0 + sp
                if den < 0:
                    off = clip(500 * (sm - sp) / den, -500, 500)
                    dt = samp[imax + 1][0] - samp[imax][0] if off > 0 else samp[imax][0] - samp[imax - 1][0]
                    tpk += off * dt / 1000.0
            w['smax'], w['smin'] = smax, smin
            w['pos_pk'] = int(clip((tpk - SW_LAT_MS) * 10 / mspp, 0, moved))
            w['over'] = moved - w['pos_pk']
            w['peak'] = n >= 3 and smax * 100 >= smin * NOPEAK_PCT and below(s_list[-1], smax, SW_FALL_END_PCT)
        else:
            self._es_check(d, ln, moved, refused)
        self._lg('# %s dir=%d len=%d spd=%x n=%d peak=%d smax=%d smin=%d moved=%d pos_pk=%d over=%d pos=%d x=%d' %
                 (tag, d, ln, speed, n, w['peak'], w['smax'], w['smin'], moved, w['pos_pk'], w['over'], self.pos, self.x))
        return w

    # -- afd_sw_stop
    @staticmethod
    def sw_stop(sim, count, s):
        if count == sim.sw_seen:
            return False
        sim.sw_seen = count
        if s > sim.sw_max:
            sim.sw_max = s
            sim.sw_below = 0
            return False
        sim.sw_below = sim.sw_below + 1 if below(s, sim.sw_max, SW_FALL_PCT) else 0
        return sim.sw_below >= SW_FALL_N


# ---- controllers --------------------------------------------------------------------------------------
class Ctl:
    def __init__(self, sim, mode):
        self.c = sim
        cfg = NEW_VARIANTS.get(mode)
        self.l1 = cfg['l1'] if cfg else RC_L1             # AFD_RC_L1
        self.rc_pad = not cfg or not cfg['nopad']         # tracking pad on rc chunks (S4 drops it)
        self.settle = bool(cfg)                           # 11.1: settled rest reading (lands + counts toward the pass max)
        self.tp = bool(cfg and cfg['tp'])                 # S3 tracked probe
        self.tp_guard = bool(cfg and cfg.get('guard'))    # (not in the spec) probe 'peak' needs a rise before the max, not a monotonic fall
        self.tp_second = bool(cfg and cfg.get('second'))  # (not in the spec) a flat first probe chunk gets a second tracked chunk
        mode = cfg['base'] if cfg else mode
        self.gtop = mode.endswith('_gtop')   # variant (patch 11 final): skip land_top when s_rest >= RC_GATE_PCT of P, else two-sided
        mode = mode.replace('_gtop', '_top2') if self.gtop else mode
        self.notop = mode.endswith('_notop')   # variant: re-catch lands at the first s_rest >= thr without land_top
        mode = mode.replace('_notop', '')
        self.top2 = mode.endswith('_top2')  # NOT in C / spec: two-sided land_top (extra variants, see ASSUMPTIONS)
        self.mode = mode.replace('_top2', '')   # 'classic' | 'classic_fix' | 'recatch'
        self.fine = sim.lens.fine
        self.one_dir = False

    # -- afd_sweep_find.  pre = AFD_SW_PRE (0 for the re-catch fallback / reverse scan)
    def sweep_find(self, pos0, d, spd, maxch, pre=SW_PRE, one_dir=False):
        c = self.c
        chunk = CHUNK[spd]
        dr, other, found = d, False, False
        target = pos0 - d * pre
        while True:
            if target != c.pos:
                c.move_ex('o', target - c.pos, 0)
            gmax = 0.0
            gmin = float('inf')
            gfirst = gtop = 0.0
            gpk = 0
            stopped, kdone = False, 0
            k = 0
            w = None
            while k < maxch and not found and not stopped:
                c.sw_max0 = gmax
                w = c.sweep('sweep', dr, chunk, spd, Sim.sw_stop)
                kdone = k + 1
                if w['n']:
                    if not gfirst:
                        gfirst = w['gfirst']
                    gtop = max(gtop, w['gmax'])
                    if w['smax'] > gmax:
                        gmax = w['smax']
                        gpk = c.pos - dr * w['over']
                    gmin = min(gmin, w['smin'])
                last = w['samp'][-1][1] if w['n'] else 0
                found = w['n'] >= 3 and gmax * 100 >= gmin * NOPEAK_PCT and below(last, gmax, SW_FALL_END_PCT)
                if w['endstop']:
                    stopped = True
                k += 1
            if found:
                break
            cap = (not other and not one_dir and maxch * chunk >= CAP_RANGE and gmax and gfirst and gmax * 100 < gmin * NOPEAK_PCT
                   and gtop * 100 <= gfirst * CAP_G_PCT and not (stopped and kdone < ES_EARLY_CHUNKS))
            if not other and not one_dir and not cap:
                other = True
                dr = -d
                target = pos0 + d * pre
                continue
            c.nopeak = True
            if c.pos != pos0:
                c.move_ex('o', pos0 - c.pos, 0)
            return None
        return dr, gmax, gpk

    # -- afd_retrace
    def retrace(self, k, thr_pct, ref, start, d, max_steps):
        c = self.c
        if start != c.pos:
            c.move_ex('o', start - c.pos, 0)
        pm, pi, hit, flat = 0.0, -1, False, False
        last = [0.0] * FLAT_RT_N
        for i in range(max_steps):
            s = c.move_ex('r', d * self.fine, 2)
            if s > pm:
                pm, pi = s, i
            if not below(s, ref, thr_pct):
                hit, pm, pi = True, s, i
                break
            if i > pi and below(s, pm, RT_FALL_PCT):
                break
            last[i % FLAT_RT_N] = s
            if c.flat_det and i + 1 >= FLAT_RT_N and (i + 1) * self.fine >= c.flat_min_travel:
                hi, lo = max(last), min(last)
                if close(hi, lo, FLAT_RT_PCT) and below(hi, ref, FLAT_RT_REF_PCT):
                    flat = True
                    break
        c._lg('# retrace k=%d hit=%d s=%d%s' % (k, hit, pm, ' flat=1' if flat else ''))
        if flat:
            c.land_flat = True
        return hit, pi, pm

    def land_top(self, d, r):
        c = self.c
        h = max(self.fine // 2, 6)
        if self.top2:
            return self.land_top2(d, r, h)
        best = r
        for _ in range(6):
            s2 = c.move_ex('t', d * h, 2)
            if s2 >= best:
                best = r = s2
                continue
            if below(s2, best, 97):
                s3 = c.move_ex('t', -d * h, 2)
                r = s3
            else:
                r = s2
            break
        return r

    def land_top2(self, d, r, h):
        """Variant, not in the C code or the spec: land_top, but when the first half step in d goes down also try the other side
        (a re-catch chunk may end just past the top, where land_top's single step back only restores the same point)."""
        c = self.c

        def climb(sd, r, n):
            for _ in range(n):
                s2 = c.move_ex('t', sd * h, 2)
                if s2 >= r:
                    r = s2
                    continue
                return c.move_ex('t', -sd * h, 2) if below(s2, r, 97) else s2
            return r
        s2 = c.move_ex('t', d * h, 2)
        if s2 >= r:
            return climb(d, s2, 5)
        s3 = c.move_ex('t', -d * h, 2)           # back at the start point
        s4 = c.move_ex('t', -d * h, 2)           # one step to the other side
        if s4 > max(s3, r):
            return climb(-d, s4, 4)
        return c.move_ex('t', d * h, 2)          # nothing better: back to the start point

    # -- afd_land_steps(_i)
    def land_steps(self, d, start, back, ref):
        c = self.c
        c.land_flat = False
        hit, land, st, bidx = False, 0.0, start, 0
        for k in (1, 2):
            if hit:
                break
            if k == 2:
                st = start - d * (RT_BACK2 - RT_BACK)
                back += RT_BACK2 - RT_BACK
            hit, bidx, land = self.retrace(k, RT_THR1 if k == 1 else RT_THR2, ref, st, d,
                                          back // self.fine + RT_SLACK + (RT_EXTRA // self.fine) * (k - 1))
            if c.land_flat:
                return 0.0
        if not hit:
            c.move_ex('o', st - c.pos, 0)
            for _ in range(bidx + 1):
                land = c.move_ex('l', d * self.fine, 2)
            if not land:
                land = c.move_ex('i', 0, 1)
        if land and not self.notop:
            land = self.land_top(d, land)
        return land

    # -- afd_land_recatch (spec section 2)
    def recatch(self, dr, P, over):
        c = self.c
        d = -dr
        thr = P * RT_THR1 / 100.0
        L = self.l1
        lmin = min(2 * self.fine, L)
        cap = over + SW_ERR + L
        pass_n = 1
        nchunks = 0
        while pass_n <= RC_PASSES:
            covered = 0
            pass_max = 0.0
            since = None
            extended = False
            passed = False
            while True:
                if nchunks >= RC_MAX_CHUNKS:
                    c.land_flat = True
                    return 0.0
                nchunks += 1
                w = c.sweep('rc', d, L, SPD07, None, pad=SW_TRACK_PAD_MS if self.rc_pad else 0)
                covered += w['moved']
                if w['n'] and w['smax'] > pass_max:
                    pass_max = w['smax']
                    since = w['over']
                elif since is not None:
                    since += w['moved']
                if self.settle:
                    s_rest, settled = c.settle_rest()
                    if settled and s_rest > pass_max:      # a settled rest reading counts toward the pass max (we are on it)
                        pass_max, since = s_rest, 0
                else:
                    s_rest, settled = c.move_ex('i', 0, 1), True
                c._lg('# recatch pass=%d d=%d L=%d covered=%d cap=%d smax=%d s_rest=%d thr=%d' %
                      (pass_n, d, L, covered, cap, pass_max, s_rest, thr))
                if settled and s_rest >= thr:
                    if self.notop or (self.gtop and s_rest >= P * RC_GATE_PCT / 100.0):
                        return s_rest
                    return self.land_top(d, s_rest)
                if settled and pass_max >= P * RC_SEEN_PCT / 100.0 and below(s_rest, pass_max, RC_PASSED_PCT):
                    cap = (since or 0) + L
                    d = -d
                    L = max(L // 2, lmin)
                    pass_n += 1
                    passed = True
                    break
                if covered >= cap:
                    if pass_max < P * RC_LOST_PCT / 100.0 or extended:
                        c.land_flat = True
                        return 0.0
                    cap += cap // 2
                    extended = True
            if passed and L == lmin and pass_n > 2:
                return self.final_walk(d, P, thr, cap)
        c.land_flat = True
        return 0.0

    def final_walk(self, d, P, thr, cap):
        c = self.c
        wmax, first = 0.0, None
        for _ in range(cap // self.fine + 3):
            s = c.move_ex('r', d * self.fine, 2)
            if first is None:
                first = s
            if s >= thr:
                return self.land_top(d, s)
            wmax = max(wmax, s)
            if wmax >= 1.2 * first and below(s, wmax, RC_PASSED_PCT):
                sb = c.move_ex('r', -d * self.fine, 2)
                if sb >= P * 0.8:
                    return sb
                break
        c.land_flat = True
        return 0.0

    # -- afd_probe_dir (go_back = 0)
    def probe_dir(self, g0):
        c = self.c
        c.endstop_dir = 0
        self.probe_flat = False
        d = 1
        c.move_ex('p', COARSE, 2)
        gc = c.last_g
        if c.endstop_dir:
            d = -c.endstop_dir
        elif below(gc, g0, PROBE_PCT):
            d = -1
        elif not gc * 100 > g0 * (200 - PROBE_PCT):
            c.move_ex('p', COARSE, 2)
            if c.endstop_dir:
                d = -c.endstop_dir
            elif not c.last_g * 100 > gc * 105:
                d = -1
                self.probe_flat = True
        return d

    # -- S3 tracked probe: the first 100-pulse tracked 07 sweep chunk is the direction probe (no jump back)
    def probe_tp(self):
        c = self.c
        c.endstop_dir = 0
        c.sw_max0 = 0.0
        w = c.sweep('probe', 1, CHUNK[SPD07], SPD07, Sim.sw_stop)
        if w['endstop']:
            return 'dir', -(c.endstop_dir or 1)
        if w['peak'] and not (self.tp_guard and w['samp'][0][1] * 100 > w['smax'] * 85):
            return 'found', (1, w['smax'], c.pos - w['over'])
        if w['n'] < 2:
            return 'flat', -1
        sf, sl = w['samp'][0][1], w['samp'][-1][1]
        gf, gl = w['gfirst'], w['samp'][-1][2]
        if self.tp_second and not (sl * 100 > sf * 105 or gl * 100 > gf * (200 - PROBE_PCT) or below(gl, gf, PROBE_PCT)
                                   or below(sl, sf, PROBE_PCT)):
            w2 = c.sweep('probe', 1, CHUNK[SPD07], SPD07, Sim.sw_stop)
            if w2['endstop']:
                return 'dir', -(c.endstop_dir or 1)
            if w2['n'] >= 2:
                if w2['peak']:
                    return 'found', (1, w2['smax'], c.pos - w2['over'])
                sl2, gl2 = w2['samp'][-1][1], w2['samp'][-1][2]
                if sl2 * 100 > sl * 105 or gl2 * 100 > gl * (200 - PROBE_PCT):
                    return 'dir', 1
            return 'flat', -1
        if sl * 100 > sf * 105 or gl * 100 > gf * (200 - PROBE_PCT):
            return 'dir', 1
        if below(gl, gf, PROBE_PCT) or below(sl, sf, PROBE_PCT):
            return 'dir', -1
        return 'flat', -1

    # -- afd_run_sweep
    def run(self):
        c = self.c
        s0 = c.move_ex('i', 0, 1)
        g0 = c.last_g
        c.move_ex('i', 0, 2)
        g0 = (g0 + c.last_g) / 2
        pos0 = c.pos
        pre_r = None
        if self.tp:
            kind, val = self.probe_tp()
            if kind == 'found':
                pre_r, d, self.probe_flat = val, 1, False
            else:
                d, self.probe_flat = val, kind == 'flat'
        else:
            d = self.probe_dir(g0)
        spd = SPD07
        maxch = SW_MAX_CHUNKS
        coarse = False
        if pre_r is not None:
            r = pre_r
        elif self.probe_flat:
            coarse = True
            r = self.sweep_find(pos0, d, SPD1F, SW_COARSE_CHUNKS)
            if r is None:
                return 'nopeak'
            cdir, cref, cpk = r
            if self.mode == 'recatch':
                over1f = max(0, cdir * (c.pos - cpk))
                mch = min(8, -(-(over1f + 300) // 100))
                r = self.sweep_find(c.pos, -cdir, spd, mch, pre=0, one_dir=True)
            else:
                r = self.sweep_find(cpk - cdir * (SW_COARSE_BACK - SW_PRE), cdir, spd, maxch, one_dir=True)
        elif self.tp:
            r = self.sweep_find(c.pos, d, spd, maxch, pre=0)      # continues from the probe's end
        else:
            r = self.sweep_find(pos0, d, spd, maxch)
        if r is None:
            return 'nopeak'
        dr, ref, pk = r
        while True:
            c._lg('# sweep peak dir=%d pk=%d ref=%d pos=%d coarse=%d' % (dr, pk, ref, c.pos, coarse))
            c.flat_det = not c.resweep_used
            if self.mode == 'recatch':
                land = self.recatch(dr, ref, max(0, dr * (c.pos - pk)))
            else:
                c.flat_min_travel = FLAT_MIN_TRAVEL_FIX if self.mode == 'classic_fix' else 0
                land = self.land_steps(dr, pk - dr * SW_MARGIN, SW_MARGIN + SW_ERR, ref)
            c.flat_det = False
            if not land and c.land_flat and not c.resweep_used:
                c.resweep_used = True
                c.land_flat = False
                if self.mode == 'recatch':
                    c._lg('# land lost -> bounded resweep chunks=%d' % SW_RESWEEP_CHUNKS)
                    r = self.sweep_find(c.pos, dr, SPD07, SW_RESWEEP_CHUNKS, pre=0)
                else:
                    c._lg('# land flat -> full resweep chunks=%d' % SW_MAX_CHUNKS)
                    r = self.sweep_find(c.pos, dr, SPD07, SW_MAX_CHUNKS)
                if r is None:
                    return 'nopeak'
                dr, ref, pk = r
                continue
            break
        return 'landed' if land else 'lost'


ALGOS = ('classic', 'classic_fix', 'recatch')
EXTRAS = ('classic_top2', 'recatch_top2', 'classic_notop', 'recatch_notop', 'recatch_gtop')
# patch-12 variants (new time model + 11.1 settled rest reading on top of recatch_gtop)
NEW_VARIANTS = {
    'recatch_gtop_tm': dict(base='recatch_gtop', l1=50, nopad=False, tp=False),       # reference: only the model changes
    'recatch_gtop_l30': dict(base='recatch_gtop', l1=30, nopad=True, tp=False),       # S4
    'recatch_gtop_l30_tp': dict(base='recatch_gtop', l1=30, nopad=True, tp=True),     # S4 + S3
    # exploratory fixes of S3 (not in the spec): rise-before-max guard on the probe peak; second chunk before 'flat'
    'recatch_gtop_l30_tpg': dict(base='recatch_gtop', l1=30, nopad=True, tp=True, guard=True),
    'recatch_gtop_l30_tpg2': dict(base='recatch_gtop', l1=30, nopad=True, tp=True, guard=True, second=True),
}
P12 = tuple(NEW_VARIANTS)
RC_GATE_PCT = 97   # recatch_gtop: s at rest >= this % of the sweep peak = already on the top, no land_top


def run_trial(lens, err, defocus, algo, seed, cell, i, log=False):
    wrng = random.Random('%s/%s/world/%d' % (seed, cell, i))
    rng = random.Random('%s/%s/%s/%d' % (seed, cell, algo, i))
    sim = Sim(lens, err, rng, defocus, wrng, log=log)
    if algo in NEW_VARIANTS:
        sim.tm, sim.frame = True, 44.0
    ctl = Ctl(sim, algo)
    try:
        why = ctl.run()
    except Abort as a:
        why = a.why
    q = sim.quality()
    return dict(ok=q >= 0.9, near=q >= 0.75, q=q, why=why, moves=sim.moves, t=sim.t / 1000.0, travel=sim.travel,
                es=sim.es_contacts, dev=sim.max_dev, sim=sim)


def field_signature(log):
    """classify a classic run log against the two field traces: 139612 = peak found by the 07 sweep, landing flat-aborted, full
    resweep ran on; 189043 = probe flat, 1F coarse scan, 07 sweep from the 1F estimate finds nothing (nopeak)."""
    txt = '\n'.join(log)
    return dict(flat=('flat=1' in txt), resweep=('full resweep' in txt), coarse=('coarse=1' in txt),
                nopeak07=('coarse=1' not in txt and 'sweep peak' not in txt))


# ---- Monte Carlo ----------------------------------------------------------------------------------------
def pct_(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(math.ceil(p / 100.0 * len(v))) - 1)] if v else 0


def run_cell(lens_key, err_key, defocus, n, seed, algos=ALGOS):
    cell = '%s/%s/%d' % (lens_key, err_key, defocus)
    out = {}
    for a in algos:
        rs = [run_trial(LENSES[lens_key], ERRS[err_key], defocus, a, seed, cell, i) for i in range(n)]
        why = {}
        for r in rs:
            why[r['why']] = why.get(r['why'], 0) + 1
        out[a] = dict(
            n=n, ok=100.0 * sum(r['ok'] for r in rs) / n, near=100.0 * sum(r['near'] for r in rs) / n,
            mv_med=statistics.median(r['moves'] for r in rs), mv_p90=pct_([r['moves'] for r in rs], 90),
            t_med=statistics.median(r['t'] for r in rs), t_p90=pct_([r['t'] for r in rs], 90),
            tr_med=statistics.median(r['travel'] for r in rs), tr_max=max(r['travel'] for r in rs),
            dev_max=max(r['dev'] for r in rs), es=sum(r['es'] for r in rs) / n,
            es_runs=100.0 * sum(1 for r in rs if r['es']) / n, why=why)
    return out


def fmt_table(rows):
    h = ('| lens | err | defocus | algo | succ % | >=75 % | moves med/p90 | time s med/p90 | travel med/max | end-stop hits/run (runs %) | outcomes |\n'
         '|---|---|---|---|---|---|---|---|---|---|---|\n')
    lines = []
    for (lk, ek, df), res in rows:
        for a in res:
            m = res[a]
            lines.append('| %s | %s | %d | %s | %.0f | %.0f | %d/%d | %.1f/%.1f | %d/%d | %.2f (%.0f) | %s |' % (
                LENSES[lk].name, ek, df, a, m['ok'], m['near'], m['mv_med'], m['mv_p90'], m['t_med'], m['t_p90'],
                m['tr_med'], m['tr_max'], m['es'], m['es_runs'],
                ' '.join('%s:%d' % kv for kv in sorted(m['why'].items()))))
    return h + '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--trials', type=int, default=300)
    ap.add_argument('--seed', default='recatch1')
    ap.add_argument('--lens', action='append', choices=sorted(LENSES))
    ap.add_argument('--err', action='append', choices=sorted(ERRS))
    ap.add_argument('--defocus', action='append', type=int)
    ap.add_argument('--trace', nargs=5, metavar=('LENS', 'ERR', 'DEFOCUS', 'I', 'ALGO'))
    ap.add_argument('--algo', action='append', help='explicit algorithm list (e.g. classic recatch_gtop %s)' % ' '.join(P12))
    ap.add_argument('--extras', action='store_true', help='also run the *_top2 variants')
    ap.add_argument('--assumptions', action='store_true')
    a = ap.parse_args(argv)
    if a.assumptions:
        print(ASSUMPTIONS)
        return 0
    if a.trace:
        lk, ek, df, i, algo = a.trace
        r = run_trial(LENSES[lk], ERRS[ek], int(df), algo, a.seed, '%s/%s/%s' % (lk, ek, df), int(i), log=True)
        print('\n'.join(r['sim'].log))
        print('# RESULT %s q=%.2f moves=%d t=%.1fs travel=%d es=%d' % (r['why'], r['q'], r['moves'], r['t'], r['travel'], r['es']))
        return 0
    rows = []
    for lk in a.lens or ('55', '18', '50w'):
        for df in a.defocus or (60, 150, 400):
            for ek in a.err or ('none', 'realistic', 'bad'):
                rows.append(((lk, ek, df), run_cell(lk, ek, df, a.trials, a.seed, tuple(a.algo) if a.algo else ALGOS + EXTRAS if a.extras else ALGOS)))
    print(fmt_table(rows))
    return 0


ASSUMPTIONS = '''Model assumptions (af_recatch.py)
1. Peak-estimate error: the lens' true start delay of a tracked chunk = 120 ms + U(-pe, pe) pulses * 2.9 ms/pulse (1F: x1.5, 1.0 ms/pulse),
   so the time->position estimate of the peak is off by exactly U(-pe,pe) inside every chunk; the chunk end position is command-exact.
2. Unmeasured (1F) moves and 1F tracked chunks: travel = c*(1+U(+-gain)) + U(+-off) per 300-pulse command, plus with probability p an extra
   +-U(80,250); 1F tracked chunks also have a true speed error U(+-f1_speed).  Measured / 07 moves: gain N(0, step_sd).
3. Backlash: per run a fixed lost motion b (uniform per Err: 0-4 realistic, 2-10 bad; field: +-40/+-80 reversals return 95-103 %) on every direction reversal, jitter sd 1.5.  Dead reckoning (afd_pos) does not know it.
4. (9,3,1) stop does not halt a tracked move (err != none): after the B sweep's stop callback the lens still travels the full chunk while
   afd_pos is corrected to the estimated stop position (the C code does this).  The re-catch sweeps use no callback.
5. Noise: sd = sqrt((1 % * floor)^2 + (2.5 % * (s - floor))^2); stable pair /sqrt(2).  3-point exposure blur of ~12 ms travel on tracked frames.
6. Curves: Gaussian s peak (sigma 12 / 21 / 45 pulses), floor 16.3k, peak 125k (ex scale), g = broad Gaussian sigma 100 (150 EF50) gaining 25 %.
   No broad pedestal by default (see REPORT notes: the real sweep traces look wider in time than the static walks).
7. Range: 2000 pulses of travel, peak 500..1500 from the low end; end stop = command clipped; fully refused chunks answer fast with 0x1000 and
   feed afd_es_check (2 in a row => give-back).  Start = peak +-defocus (sign random, unknown to the controller).
8. Times: measured step 120+2.9|n| (510 floor for >=50) + 112 ms; unmeasured 250+|n|(+150 per extra 300) + 90 ms; tracked chunk
   max(track_end, true end)+40 ms (~560-600 ms); stable-pair +60 ms.  Run cap 18 s (AFD_RUN_CAP_B_MS), travel cap 9000.
Simplifications: g is not used for stopping; AFD_FINE is the focal-length table value (no learned fine step); no luma/LV effects;
the "infocus" shortcut of the 1F scan is off; hunt direction forcing is off; afd_to_ex scale conversions omitted (single scale).
Patch-12 variants (NEW_VARIANTS; the older variants keep the model above): measured 07 move done = 153 + 1.33|p| ms, 31..100 pulse moves hit
the 510 ms timer 50 % (94 % on a reversal), <= 30 pulses 2 %; LV frame 44 ms; re-catch chunks use no tracking pad (L30) and are followed by a
settled rest reading (frame sync + frames until 3 agree within 5 %, <= 12; residual ring-down U(+-8) pulses halving per frame) that alone can
land the re-catch and counts toward the pass max.  S3 probe = first tracked 100-pulse chunk (no jump back).  S2 (skip Local) is NOT modelled:
the simulator has no Local phase.  *_tpg / *_tpg2 are exploratory fixes of S3 (see test_recatch.TestPatch12).
Re-catch: "rise then fall" in the final fine walk is implemented as walk max >= 1.2 x first sample then s < 85 % of walk max
(unspecified in the spec); the bounded fallback sweeps first in the original sweep direction, then the other way from its start.
'''

if __name__ == '__main__':
    sys.exit(main())
