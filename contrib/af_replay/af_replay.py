#!/usr/bin/env python3
"""af_replay: offline replay / simulation of autofocus algorithms for the Canon EOS M ML
project (crop_rec "AF drive (MPU)"), driven by real focus curves from the camera logs.

  python3 af_replay.py --logs <dir> [--trials N] [--report path]

Part 1 parses the logs (AFDRIVE / AFCALIB / AFPROBE / AFSPEED / MPUDRIVE / AFSEARCH and the
future AFSWEEP), extracts same-direction (pos, s, g) series and fits
    B + A*exp(-(p-p*)^2 / (2 sigma^2))
Part 2 is a lens simulator (relative pulses, reversal offset, move timing, noise) and six
algorithms (A..F) over a small camera API.  Part 3 runs Monte Carlo trials and writes
REPORT.md.  Python 3 standard library only (no numpy needed).
"""
import argparse, glob, math, os, random, re, statistics, sys, time
from dataclasses import dataclass, field, replace

# ----------------------------------------------------------------------------------------
# small numeric helpers (stdlib only)
# ----------------------------------------------------------------------------------------

def pct(vals, p):
    if not vals:
        return float('nan')
    v = sorted(vals)
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def med(vals):
    return pct(vals, 50)


def lstsq(rows, ys):
    """Least squares via normal equations (small systems). rows: list of feature lists."""
    m = len(rows[0])
    ata = [[sum(r[i] * r[j] for r in rows) for j in range(m)] for i in range(m)]
    atb = [sum(r[i] * y for r, y in zip(rows, ys)) for i in range(m)]
    for i in range(m):                       # gauss-jordan with partial pivoting
        piv = max(range(i, m), key=lambda k: abs(ata[k][i]))
        ata[i], ata[piv] = ata[piv], ata[i]
        atb[i], atb[piv] = atb[piv], atb[i]
        d = ata[i][i] or 1e-12
        for j in range(i, m):
            ata[i][j] /= d
        atb[i] /= d
        for k in range(m):
            if k != i:
                f = ata[k][i]
                for j in range(i, m):
                    ata[k][j] -= f * ata[i][j]
                atb[k] -= f * atb[i]
    return atb


# ----------------------------------------------------------------------------------------
# Part 1a: log parsing
# ----------------------------------------------------------------------------------------

MOVE_RE = re.compile(r'^(\d+)\s+([a-zA-Z])\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-f]+)\s+(\S+)\s+(-?\d+)\s+ret(-?\d+)\s+dist=(\d+)(.*)$')
HDR_RE = re.compile(r'^# (AFDRIVE|AFCALIB|AFSPEED|AFPROBE|AFSEARCH|MPUDRIVE|AFSWEEP)\b(.*)$')


def parse_move_log(path):
    """AFDRIVE/AFCALIB/AFPROBE/AFSPEED: list of runs; a run = dict(kind, header, moves, comments).
    A move = dict(ms, ph, cmd, pos, s, g, luma, done_val, flags, done_ms, extra)."""
    runs, cur = [], None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            h = HDR_RE.match(line)
            if h:
                cur = dict(kind=h.group(1), header=line, moves=[], comments=[], file=path)
                runs.append(cur)
                continue
            if cur is None:
                continue
            if line.startswith('#'):
                cur['comments'].append((len(cur['moves']), line))
                continue
            m = MOVE_RE.match(line)
            if m:
                g = m.groups()
                cur['moves'].append(dict(ms=int(g[0]), ph=g[1], cmd=int(g[2]), pos=int(g[3]), s=int(g[4]), g=int(g[5]),
                                         luma=int(g[6]), done_val=g[7], flags=g[8], done_ms=int(g[9]), extra=g[12]))
    return runs


def parse_search_like(path):
    """AFSEARCH / MPUDRIVE sample blocks: list of lists of (ms, s, g). Only used for noise."""
    blocks, cur = [], None
    pat = re.compile(r'^(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)(\s+\w+)?$')
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#') and ('ms s g' in line):
                cur = []
                blocks.append(cur)
                continue
            if cur is None:
                continue
            m = pat.match(line)
            if m:
                cur.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
            elif not line.startswith('#') and line.strip() and not re.match(r'^\d+ (GATES|SEND|STEP|RESULT|MONOPEN|SESSION)', line):
                pass
    return blocks


def parse_mpudrive_sweeps(path, lag_ms=42.0):
    """MPUDRIVE.LOG steps with the 0x07 speed byte are already continuous sweeps: samples 'ms s g' are timed from
    the send, FOCUS_DONE at the RESULT line.  Treated like AFSWEEP: pos = cmd*(t-lag)/T (T = RESULT - SEND time)."""
    out = []
    t_send = t_res = cmd = None
    in_blk, samples = False, []
    step_re = re.compile(r'^(\d+) STEP \d+ "spd(\w\w) ([+-]\d+)"')

    def flush():
        if cmd is not None and t_send is not None and t_res is not None and len(samples) >= 6 and abs(cmd) >= 100:
            T = float(t_res - t_send)
            pos = [cmd * (t - lag_ms) / T for t, s_, g_ in samples]
            keep = [i for i, p_ in enumerate(pos) if (0 <= p_ / cmd <= 1.0)]
            if len(keep) >= 6:
                out.append(dict(kind='MPUDRIVE', step=0, ph='sweep', file=path, cmd=cmd, T=T,
                                pos=[pos[i] for i in keep], s=[samples[i][1] for i in keep], g=[samples[i][2] for i in keep],
                                t=[samples[i][0] for i in keep]))
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            m = step_re.match(line)
            if m:
                flush()
                samples, in_blk, t_send, t_res = [], False, None, None
                cmd = int(m.group(3)) if m.group(2) == '07' else None
                continue
            if re.match(r'^\d+ SEND ', line):
                t_send = int(line.split()[0])
            elif re.match(r'^\d+ RESULT ', line):
                t_res = int(line.split()[0])
            elif line.startswith('# ms s g'):
                in_blk, samples = True, []
            elif in_blk:
                mm = re.match(r'^(\d+) (\d+) (\d+) (\d+) (\d+)$', line)
                if mm:
                    samples.append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3))))
                else:
                    in_blk = False
    flush()
    return out


# ---- AFSWEEP (format arrives later; supported now) ---------------------------------------
# # AFSWEEP t=.. lens=".." zoom=x5 frame=42 ...
# # sweep k=1 cmd=600 speed=7 dir=1 [t=<ms of the send>]
# S <t_ms> <frame> <s1> <s2> <s4> <g> <q> <band> <luma>      (documented future format)
# S <t_ms> <s1> <g> <luma>                                   (shipped crop_rec.c format)
# # done k=1 t=<ms of FOCUS_DONE> val=<hex> n=<samples>
# t_ms is on one clock.  t_start = the sweep header's t= if present, else the first S line's t.
# pos(t) = cmd * (t - t_start - lag) / (t_done - t_start).   cmd is made signed by dir.

def _kv(line):
    return dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))


def parse_afsweep(path_or_text, text=False):
    if text:
        lines = path_or_text.splitlines()
    else:
        with open(path_or_text, errors='replace') as f:
            lines = f.read().splitlines()
    head, sweeps, cur = {}, [], None
    for line in lines:
        if line.startswith('# AFSWEEP'):
            head = _kv(line)
        elif line.startswith('# sweep'):
            kv = _kv(line)
            cmd = int(kv.get('cmd', 0))
            d = int(kv.get('dir', 1))
            cur = dict(k=int(kv.get('k', len(sweeps) + 1)), cmd=cmd if cmd < 0 else cmd * (1 if d >= 0 else -1),
                       speed=kv.get('speed'), dir=d, t_start=float(kv['t']) if 't' in kv else None, samples=[], done=None)
            sweeps.append(cur)
        elif line.startswith('S ') and cur is not None:
            p = line.split()
            if len(p) >= 10:
                # documented future/full format: S t frame s1 s2 s4 g q band luma
                cur['samples'].append(dict(t=float(p[1]), frame=int(p[2]), s1=float(p[3]), s2=float(p[4]), s4=float(p[5]),
                                           g=float(p[6]), q=float(p[7]), band=p[8], luma=float(p[9])))
            elif len(p) == 5:
                # shipped crop_rec.c afw_log_samples() format: S t_ms s1 g luma (4 fields, t in ms)
                cur['samples'].append(dict(t=float(p[1]), frame=0, s1=float(p[2]), s2=float(p[2]), s4=float(p[2]),
                                           g=float(p[3]), q=0.0, band='-', luma=float(p[4])))
        elif line.startswith('# done') and cur is not None:
            kv = _kv(line)
            cur['done'] = dict(t=float(kv['t']), val=kv.get('val'), n=int(kv.get('n', 0)))
    for sw in sweeps:
        if sw['t_start'] is None and sw['samples']:
            sw['t_start'] = sw['samples'][0]['t']
    return head, sweeps


def sweep_series(sweep, lag_ms=42.0, which='s1'):
    """-> (pos list, s list, g list) for one parsed sweep (positions from time)."""
    if not sweep['done'] or not sweep['samples']:
        return [], [], []
    t0, t1 = sweep['t_start'], sweep['done']['t']
    T = max(t1 - t0, 1.0)
    pos, s, g = [], [], []
    for smp in sweep['samples']:
        f = (smp['t'] - t0 - lag_ms) / T
        if f < 0 or f > 1.0:
            continue
        pos.append(sweep['cmd'] * f)
        s.append(smp[which])
        g.append(smp['g'])
    return pos, s, g


def synthetic_afsweep(rng, cmd=600, sig=25.0, ab=1.5, sig_g=100.0, ab_g=1.2, T=1500.0, t_start=10000.0, frame=42.0, lag=42.0,
                      peak=300.0, noise=0.01, floor_s=20000.0, floor_g=25000.0):
    """Generate AFSWEEP text from a known curve (for testing the parser and the time->pos conversion)."""
    out = ['# AFSWEEP t=%d lens="EF-M18-55mm" zoom=x5 frame=%d' % (t_start - 50, frame),
           '# sweep k=1 cmd=%d speed=7 dir=%d t=%d' % (abs(cmd), 1 if cmd > 0 else -1, t_start)]
    t, fr, n = t_start + rng.uniform(0, frame), 0, 0
    while t < t_start + T + 60:
        f = min(max((t - t_start - lag) / T, 0), 1)
        x = cmd * f
        s = floor_s * (1 + ab * math.exp(-(x - peak) ** 2 / (2 * sig ** 2))) * (1 + rng.gauss(0, noise))
        g = floor_g * (1 + ab_g * math.exp(-(x - peak) ** 2 / (2 * sig_g ** 2))) * (1 + rng.gauss(0, noise))
        out.append('S %d %d %d %d %d %d %d %s %d' % (t, fr, s, s * 0.8, s * 0.6, g, 500, 'M', 100))
        t += frame
        fr += 1
        n += 1
    out.append('# done k=1 t=%d val=4e0400 n=%d' % (t_start + T, n))
    return '\n'.join(out) + '\n'


def selftest_parser(verbose=False):
    """Round-trip: synthetic sweep -> parser -> series -> Gaussian fit; peak position and sigma must come back."""
    rng = random.Random(1)
    ok = True
    res = []
    for cmd in (600, -600):
        peak = 300.0 if cmd > 0 else -250.0
        txt = synthetic_afsweep(rng, cmd=cmd, peak=peak, sig=25.0)
        head, sweeps = parse_afsweep(txt, text=True)
        assert len(sweeps) == 1 and sweeps[0]['cmd'] == cmd and sweeps[0]['done']['n'] == len(sweeps[0]['samples'])
        pos, s, g = sweep_series(sweeps[0], lag_ms=42.0)
        fs = fit_gauss(pos, s)
        fg = fit_gauss(pos, g)
        good = fs is not None and abs(fs['p'] - peak) < 8 and abs(fs['sig'] - 25) < 8 and fg is not None and abs(fg['p'] - peak) < 20
        ok &= good
        res.append((cmd, len(pos), None if fs is None else (round(fs['p'], 1), round(fs['sig'], 1)), None if fg is None else (round(fg['p'], 1), round(fg['sig'], 1)), good))
    if verbose:
        for r in res:
            print('  parser selftest cmd=%d n=%d fit_s(p*,sig)=%s fit_g=%s %s' % (r[0], r[1], r[2], r[3], 'ok' if r[4] else 'FAIL'))
    return ok, res


# ----------------------------------------------------------------------------------------
# Part 1b: Gaussian curve fit  y = B + A*exp(-(p-p*)^2/(2 sig^2))
# ----------------------------------------------------------------------------------------

def _gfit_at(pos, y, p, sig, bmin, bmax):
    n = len(pos)
    e = [math.exp(-(x - p) ** 2 / (2 * sig * sig)) for x in pos]
    se, sy = sum(e), sum(y)
    see = sum(v * v for v in e)
    sey = sum(a * b for a, b in zip(e, y))
    den = n * see - se * se
    if den < 1e-9:
        return None
    A = (n * sey - se * sy) / den
    B = (sy - A * se) / n
    if B < bmin or B > bmax:
        B = min(max(B, bmin), bmax)
        A = sum(a * (b - B) for a, b in zip(e, y)) / max(see, 1e-9)
    if A < 0:
        A = 0.0
    sse = sum((B + A * a - b) ** 2 for a, b in zip(e, y))
    return sse, B, A


def fit_gauss(pos, y, sig_lo=5.0, sig_hi=300.0):
    """Grid search + local refinement over (p*, sig); B, A by linear LS, B constrained to [0.4, 1.0]*min(y)."""
    n = len(pos)
    if n < 5:
        return None
    ymin, ymax = min(y), max(y)
    if ymax <= 0:
        return None
    bmin, bmax = 0.4 * ymin, ymin
    sst = sum((v - sum(y) / n) ** 2 for v in y)
    if sst <= 0:
        return None
    pmin, pmax = min(pos), max(pos)
    sigs = [sig_lo * (sig_hi / sig_lo) ** (i / 23.0) for i in range(24)]
    best = None
    step = max(2.0, (pmax - pmin) / 80.0)
    p = pmin - 30
    while p <= pmax + 30:
        for sg in sigs:
            r = _gfit_at(pos, y, p, sg, bmin, bmax)
            if r and (best is None or r[0] < best[0]):
                best = (r[0], p, sg, r[1], r[2])
        p += step
    if best is None:
        return None
    for _ in range(2):                           # refine
        _, p0, s0, _, _ = best
        for dp in [(i - 6) * step / 6.0 for i in range(13)]:
            for ds in [0.8 + 0.05 * i for i in range(9)]:
                r = _gfit_at(pos, y, p0 + dp, s0 * ds, bmin, bmax)
                if r and r[0] < best[0]:
                    best = (r[0], p0 + dp, s0 * ds, r[1], r[2])
    sse, p, sg, B, A = best
    return dict(p=p, sig=sg, B=B, A=A, ab=A / B if B else float('inf'), r2=1 - sse / sst, n=n)


def accept_fit(fit, pos, min_r2=0.85, min_ab=0.2):
    """A fit is a usable curve if it explains the data, has a real peak inside the series and a sane sigma."""
    if fit is None:
        return False
    if fit['r2'] < min_r2 or fit['ab'] < min_ab:
        return False
    if not (min(pos) - 10 <= fit['p'] <= max(pos) + 10):
        return False
    if fit['sig'] < 5.5 or fit['sig'] > 290:
        return False
    return True


# ----------------------------------------------------------------------------------------
# Part 1c: series extraction and log statistics
# ----------------------------------------------------------------------------------------

def sign(x):
    return (x > 0) - (x < 0)


def extract_series(run):
    """Same-direction runs of measured moves.  A reversal move shifts every following point by the same
    (unknown) offset, so a series = a reversal move plus the same-direction moves that follow it; positions
    inside it are mutually consistent.  Broken by: direction change, unmeasured ('o', s==0) moves, phase
    letter change (u/d passes), baseline ('i') lines."""
    series, cur, curph = [], [], None
    for mv in run['moves']:
        if mv['cmd'] == 0 or mv['s'] == 0 or mv['ph'] == 'o' or 'nodone' in mv['flags'] or 'err' in mv['flags']:
            if cur:
                series.append(cur)
            cur, curph = [], None
            continue
        d = sign(mv['cmd'])
        if cur and (d != sign(cur[-1]['cmd']) or mv['ph'] != curph or abs(mv['cmd']) != abs(cur[-1]['cmd'])):
            series.append(cur)
            cur = []
        cur.append(mv)
        curph = mv['ph']
    if cur:
        series.append(cur)
    out = []
    for ser in series:
        if len(ser) >= 5:
            out.append(dict(kind=run['kind'], step=abs(ser[0]['cmd']), ph=ser[0]['ph'], file=run['file'],
                            pos=[m['pos'] for m in ser], s=[m['s'] for m in ser], g=[m['g'] for m in ser]))
    return out


def solve_move_time(runs):
    """FOCUS_DONE time per move.  Two modes are visible in the logs:
       fast  - done_val bit 0x0400 clear: done_ms ~ a + b*|n|
       timer - bit 0x0400 set: done_ms ~ 510 ms whatever the size (a completion timer?)
    The fraction of timer-mode completions grows with |n|.  Also fits the plain single line a0 + b0*|n| over all
    moves (what the brief asks for) and an extra reversal term."""
    fast, allm, bins = [], [], {}
    edges = (20, 40, 60, 90, 150, 100000)
    for run in runs:
        if run['kind'] == 'AFSPEED':
            continue          # mixed speeds
        last = 0
        for mv in run['moves']:
            n = mv['cmd']
            if n == 0:
                continue
            d = sign(n)
            rev = 1 if (last and d != last) else 0
            last = d
            if mv['flags'] != '-' or mv['done_ms'] <= 0:
                continue
            slow = (int(mv['done_val'], 16) >> 8) & 0x4 != 0
            allm.append((abs(n), rev, mv['done_ms']))
            k = next(i for i, e in enumerate(edges) if abs(n) <= e)
            bins.setdefault(k, [0, 0])
            bins[k][0] += 1
            bins[k][1] += 1 if slow else 0
            if not slow:
                fast.append((abs(n), rev, mv['done_ms']))
    if len(fast) < 10:
        return None
    a, b = lstsq([[1.0, n] for n, r, t in fast], [t for n, r, t in fast])
    res = [t - (a + b * n) for n, r, t in fast]
    a0, b0 = lstsq([[1.0, n] for n, r, t in allm], [t for n, r, t in allm])
    ar, br, cr = lstsq([[1.0, n, r] for n, r, t in allm], [t for n, r, t in allm])
    slow_ms = [t for n, r, t in allm if t >= 495]
    return dict(a=a, b=b, fast_n=len(fast), res_sd=statistics.pstdev(res), a0=a0, b0=b0, n=len(allm),
                all_with_rev=(ar, br, cr),
                p_slow=[(edges[k], bins[k][0], bins[k][1] / bins[k][0]) for k in sorted(bins)],
                slow_ms=med(slow_ms) if slow_ms else 510.0)


def solve_overheads(runs):
    """gap = ms_i - ms_(i-1) - done_ms: settle wait + sample.  Single sample phases (p,c,f,o-measured) vs pair (r,l,u,d)."""
    single, pair, bare = [], [], []
    for run in runs:
        if run['kind'] not in ('AFDRIVE', 'AFCALIB'):
            continue
        mv = run['moves']
        for i in range(1, len(mv)):
            a, b = mv[i - 1], mv[i]
            if b['cmd'] == 0 or b['done_ms'] < 0:
                continue
            gap = b['ms'] - a['ms'] - b['done_ms']
            if gap < 0 or gap > 400:
                continue
            if b['s'] == 0:
                bare.append(gap)
            else:
                (pair if b['ph'] in ('r', 'l', 'u', 'd') else single).append(gap)
    return dict(single=med(single) if single else None, pair=med(pair) if pair else None,
                bare=med(bare) if bare else None, n1=len(single), n2=len(pair), n3=len(bare))


def solve_noise(runs, blocks):
    """Relative sd of one s reading (static scene)."""
    est = {}
    # (a) AFDRIVE: 'i' line vs the second reading s1 = 2*s0 - s_i (s0 is their mean, logged in '# s0=')
    pairs = []
    for run in runs:
        if run['kind'] != 'AFDRIVE':
            continue
        for idx, c in run['comments']:
            m = re.match(r'# s0=(\d+) g=(\d+)', c)
            if m and idx >= 1 and run['moves'][idx - 1]['ph'] == 'i':
                si = run['moves'][idx - 1]['s']
                s0 = int(m.group(1))
                s1 = 2 * s0 - si
                if s0 > 0 and s1 > 0:
                    pairs.append((si - s1) / (0.5 * (si + s1)))
    # (b) AFSPEED consecutive 'i' lines at the same position
    for run in runs:
        if run['kind'] == 'AFSPEED':
            mv = run['moves']
            for i in range(1, len(mv)):
                if mv[i]['ph'] == 'i' and mv[i - 1]['ph'] == 'i' and mv[i]['s'] and mv[i - 1]['s']:
                    pairs.append((mv[i]['s'] - mv[i - 1]['s']) / (0.5 * (mv[i]['s'] + mv[i - 1]['s'])))
    if pairs:
        est['i-pairs (sd)'] = (statistics.pstdev(pairs) / math.sqrt(2), len(pairs))
        est['i-pairs (MAD)'] = (1.4826 * med([abs(x - med(pairs)) for x in pairs]) / math.sqrt(2), len(pairs))
    # (c) static blocks (AFSEARCH start, MPUDRIVE tails): consecutive relative differences below 8% (excludes real signal)
    diffs = []
    for blk in blocks:
        for i in range(1, len(blk)):
            a, b = blk[i - 1][1], blk[i][1]
            if a > 0 and b > 0:
                d = (b - a) / (0.5 * (a + b))
                if abs(d) < 0.08:
                    diffs.append(d)
    if diffs:
        est['static-blocks (sd)'] = (statistics.pstdev(diffs) / math.sqrt(2), len(diffs))
        est['static-blocks (MAD)'] = (1.4826 * med([abs(x - med(diffs)) for x in diffs]) / math.sqrt(2), len(diffs))
    return est


def load_logs(logdir):
    files = sorted(glob.glob(os.path.join(logdir, '**', '*.LOG'), recursive=True))
    runs, blocks, sweeps, nfiles, mpusweeps = [], [], [], {}, []
    for fp in files:
        base = os.path.basename(fp).upper()
        nfiles[base] = nfiles.get(base, 0) + 1
        if base in ('AFDRIVE.LOG', 'AFCALIB.LOG', 'AFPROBE.LOG', 'AFSPEED.LOG'):
            runs += parse_move_log(fp)
        elif base in ('AFSEARCH.LOG', 'MPUDRIVE.LOG'):
            blocks += parse_search_like(fp)
            if base == 'MPUDRIVE.LOG':
                mpusweeps += parse_mpudrive_sweeps(fp)
        elif base == 'AFSWEEP.LOG':
            head, sw = parse_afsweep(fp)
            sweeps.append((fp, head, sw))
    return dict(runs=runs, blocks=blocks, sweeps=sweeps, nfiles=nfiles, mpusweeps=mpusweeps)


def fit_all_series(data, lag_ms=42.0):
    """-> list of dict(series info, fs, fg, ok_s, ok_g)"""
    out = []
    for run in data['runs']:
        if run['kind'] not in ('AFDRIVE', 'AFCALIB'):
            continue
        for ser in extract_series(run):
            out.append(ser)
    for ms_ in data.get('mpusweeps', []):
        out.append(dict(ms_))
    for fp, head, sweeps in data['sweeps']:
        for sw in sweeps:
            pos, s, g = sweep_series(sw, lag_ms=lag_ms)
            if len(pos) >= 8:
                out.append(dict(kind='AFSWEEP', step=0, ph='sweep', file=fp, pos=pos, s=s, g=g))
    for ser in out:
        spans_s = ser['step'] <= 60
        ser['fs'] = fit_gauss(ser['pos'], ser['s']) if (spans_s or ser['kind'] in ('AFSWEEP', 'MPUDRIVE')) else None
        ser['fg'] = fit_gauss(ser['pos'], ser['g']) if (ser['step'] <= 100) else None
        ser['ok_s'] = accept_fit(ser['fs'], ser['pos'])
        ser['ok_g'] = accept_fit(ser['fg'], ser['pos'])
    return out


def curve_pool(fits):
    """Curves for the simulator: (sig_s, ab_s, sig_g, ab_g).  Prefer series where both fits are good; where only
    s is good, the g half is synthesised from the median g/s width and amplitude ratios of the joint ones."""
    fits = [x for x in fits if x['kind'] != 'MPUDRIVE']    # MPUDRIVE sweep timing is timer-limited: not trusted for widths
    joint = [x for x in fits if x['ok_s'] and x['ok_g']]
    pool = [dict(sig_s=x['fs']['sig'], ab_s=x['fs']['ab'], sig_g=x['fg']['sig'], ab_g=x['fg']['ab']) for x in joint]
    only_s = [x for x in fits if x['ok_s'] and not x['ok_g']]
    if joint:
        rw = med([x['fg']['sig'] / x['fs']['sig'] for x in joint])
        ra = med([x['fg']['ab'] / x['fs']['ab'] for x in joint])
    else:
        rw, ra = 3.0, 0.8
    for x in only_s:
        pool.append(dict(sig_s=x['fs']['sig'], ab_s=x['fs']['ab'], sig_g=min(x['fs']['sig'] * rw, 250), ab_g=x['fs']['ab'] * ra))
    return pool


# ----------------------------------------------------------------------------------------
# Part 2: the simulated camera
# ----------------------------------------------------------------------------------------

B_S, B_G = 20000.0, 25000.0          # floors (cosmetic scale, only ratios matter)


@dataclass
class Params:
    R: float = 60.0                  # reversal offset: uniform in [-R, +R] pulses, drawn per reversal
    stop_lat: float = 60.0           # ms from stop() to the lens standing still
    frame: float = 42.0              # LV frame period
    gap_bare: float = 86.0           # ms after FOCUS_DONE of an unmeasured move (fitted: median gap)
    gap_single: float = 165.0        # ... of a move followed by one sample
    gap_pair: float = 190.0          # ... followed by a stable pair
    a_fast: float = 178.0            # fast-mode FOCUS_DONE: a + b*|n|, sd
    b_fast: float = 0.72
    sd_fast: float = 95.0
    p_slow: tuple = ((20, 0.16), (40, 0.14), (60, 0.46), (90, 0.66), (150, 0.84), (100000, 0.73))
    slow_ms: float = 510.0           # timer-mode FOCUS_DONE
    sweep_v: float = 0.40            # pulses/ms during one long move.  NOT measured: AFSWEEP.LOG will tell
    start_delay: float = 30.0        # ms between the send and the first motion (true; unknown to the algorithm)
    lag_true: float = 42.0           # image lag: a frame shows the lens position one frame earlier
    lag_err: float = 0.0             # assumed lag = lag_true - lag_err
    vel_err: float = 0.10            # relative error of the algorithm's velocity estimate
    noise: float = 0.010             # relative sd of one s/g reading
    g_wide_w: float = 0.4            # g = core + wide component (the brief: g broad, about +-120)
    g_wide_sig: float = 110.0
    g_off_sd: float = 15.0           # g peak offset from the s peak (sd)
    cap_ms: float = 15000.0          # AFD_RUN_CAP_MS
    max_travel: float = 1500.0       # AFD_MAX_TRAVEL (coarse phase)
    max_total: float = 6000.0        # AFD_MAX_TOTAL
    # algorithm constants (crop_rec.c)
    COARSE: int = 100
    FINE: int = 20
    APPROACH: int = 120
    RT_BACK: int = 60
    RT_BACK2: int = 140
    RT_THR1: int = 90
    RT_THR2: int = 80
    RT_SLACK: int = 4
    RT_EXTRA: int = 40
    RT_FALL: int = 80
    FINE_MAX: int = 16
    FINE_DROP: int = 70
    FINE_DROP_N: int = 2
    NOPEAK: int = 115
    PROBE: int = 95
    C_STOP: int = 80
    C_DROP: int = 90
    C_DROP_N: int = 2
    FLAT: int = 3
    FLAT_N: int = 3
    MAX_COARSE: int = 25


class AFTimeout(Exception):
    pass


def p_slow_of(P, n):
    for e, p in P.p_slow:
        if n <= e:
            return p
    return P.p_slow[-1][1]


def sgn(x):
    return 1 if x > 0 else (-1 if x < 0 else 0)


class Cam:
    """Simulated lens + sensor.  The algorithm sees only move/measure/begin/next_sample/stop and its own commands."""

    def __init__(self, P, rng, curve, x0, noise_mult=1.0):
        self.P, self.rng, self.c = P, rng, curve
        self.x = float(x0)                       # true lens position; the s peak is at 0
        self.t = 0.0
        self.moves = 0
        self.travel = 0.0
        self.last_dir = rng.choice((-1, 1))      # unknown to the algorithm
        self.gpk = rng.gauss(0, P.g_off_sd)
        self.nf = P.noise * noise_mult
        self.v_alg = P.sweep_v * (1 + rng.gauss(0, P.vel_err))
        self.lag_alg = P.lag_true - P.lag_err
        c = curve
        w, sw = P.g_wide_w, P.g_wide_sig
        self.sig_cal = math.sqrt((1 - w) * c['sig_g'] ** 2 + w * sw ** 2) * (1 + rng.gauss(0, 0.10))   # calibrated g width
        self.bg_known = B_G * (1 + rng.gauss(0, 0.15))                                                 # floor from history

    # --- true curves ---
    def s_true(self, x):
        c = self.c
        return B_S * (1 + c['ab_s'] * math.exp(-x * x / (2 * c['sig_s'] ** 2)))

    def g_true(self, x):
        c, P = self.c, self.P
        u = x - self.gpk
        core = math.exp(-u * u / (2 * c['sig_g'] ** 2))
        wide = math.exp(-u * u / (2 * P.g_wide_sig ** 2))
        return B_G * (1 + c['ab_g'] * ((1 - P.g_wide_w) * core + P.g_wide_w * wide))

    def quality(self):
        return self.s_true(self.x) / self.s_true(0.0)

    def _read(self, x, k=1):
        nf = self.nf / math.sqrt(k)
        return self.s_true(x) * (1 + self.rng.gauss(0, nf)), self.g_true(x) * (1 + self.rng.gauss(0, nf))

    def _check(self):
        if self.t > self.P.cap_ms:
            raise AFTimeout()

    # --- stepped moves ---
    def _reversal(self, n):
        d = sgn(n)
        rev = self.last_dir != 0 and d != self.last_dir
        return (self.rng.uniform(-self.P.R, self.P.R) if rev else 0.0)

    def move(self, n, speed=7):
        """relative pulse move; returns after FOCUS_DONE + the bare settle gap."""
        P = self.P
        n = int(n)
        if n == 0:
            return 0.0
        delta = self._reversal(n)
        if self.rng.random() < p_slow_of(P, abs(n)):
            dur = P.slow_ms + abs(self.rng.gauss(0, 12))
        else:
            dur = max(40.0, self.rng.gauss(P.a_fast + P.b_fast * abs(n), P.sd_fast))
        self.x += n + delta
        self.t += dur + P.gap_bare
        self.moves += 1
        self.travel += abs(n)
        self.last_dir = sgn(n)
        self._check()
        return dur

    def measure(self, k=1):
        """k = 1 single sample, 2 = stable pair (mean).  Returns (s, g)."""
        P = self.P
        self.t += (P.gap_single if k == 1 else P.gap_pair) - P.gap_bare
        self._check()
        return self._read(self.x, k)

    # --- continuous moves ---
    def begin(self, n):
        P, r = self.P, self.rng
        delta = self._reversal(n)
        T = P.start_delay + abs(n) / P.sweep_v * (1 + r.gauss(0, 0.05))
        return dict(n=n, eff=n + delta, x0=self.x, t0=self.t, T=T, k=0, ph=r.uniform(0, P.frame), done=False)

    def _x_at(self, h, tau):
        f = (tau - self.P.start_delay) / (h['T'] - self.P.start_delay)
        return h['x0'] + h['eff'] * min(max(f, 0.0), 1.0)

    def next_sample(self, h):
        """next LV frame during the move: (tau_ms, s, g) or None when the move is over (FOCUS_DONE)."""
        P = self.P
        tau = h['ph'] + h['k'] * P.frame
        h['k'] += 1
        if tau >= h['T']:
            self._finish(h)
            return None
        self.t = h['t0'] + tau
        h['tau'] = tau
        self._check()
        s, g = self._read(self._x_at(h, tau - P.lag_true))
        return tau, s, g

    def _finish(self, h):
        P = self.P
        self.x = h['x0'] + h['eff']
        self.t = h['t0'] + h['T'] + P.gap_bare
        self.moves += 1
        self.travel += abs(h['n'])
        self.last_dir = sgn(h['n'])
        h['done'] = True
        h['tau_end'] = h['T']
        self._check()

    def stop(self, h):
        """stop() at the current frame time; the lens coasts for stop_lat ms.  Returns the algorithm's estimate
        of the distance travelled (signed), from its own velocity model."""
        P = self.P
        tau_stop = h.get('tau', 0.0) + P.stop_lat
        if tau_stop >= h['T']:
            self._finish(h)
            return h['n']
        f = min(max((tau_stop - P.start_delay) / (h['T'] - P.start_delay), 0.0), 1.0)
        self.x = h['x0'] + h['eff'] * f
        self.t = h['t0'] + tau_stop + P.gap_bare
        self.moves += 1
        self.travel += abs(h['eff'] * f)
        self.last_dir = sgn(h['n'])
        h['done'] = True
        h['tau_end'] = tau_stop
        self._check()
        return sgn(h['n']) * min(abs(h['n']), self.v_alg * tau_stop)

    def est_img_pos(self, h, tau):
        """algorithm's estimate of the lens position (signed travel from the move start) that a frame at tau shows"""
        return sgn(h['n']) * min(abs(h['n']), self.v_alg * max(tau - self.lag_alg, 0.0))


# ----------------------------------------------------------------------------------------
# algorithms (shared pieces mirror crop_rec.c afd_run / afd_retrace)
# ----------------------------------------------------------------------------------------

class Ctl:
    """The algorithm's own bookkeeping: commanded position (afd_pos) and a few helpers."""

    def __init__(self, cam, P):
        self.cam, self.P, self.pos = cam, P, 0.0
        self.last_g = 0.0

    def mv(self, n, k=1):
        """move n pulses; k=0 unmeasured, 1 single sample, 2 stable pair -> (s, g) or None"""
        self.cam.move(n)
        self.pos += n
        if k == 0:
            return None
        s, g = self.cam.measure(k)
        self.last_g = g
        return s, g

    def goto_move(self, target, approach_dir):
        """afd_goto: last movement in approach_dir; measured single sample at the end."""
        P = self.P
        need = target - self.pos
        if need == 0:
            return self.cam.measure(1)
        if need * approach_dir >= P.APPROACH:
            return self.mv(need, 1)
        self.mv(need - approach_dir * P.APPROACH, 0)
        return self.mv(approach_dir * P.APPROACH, 1)


def below(a, b, pct_):
    return a * 100 < b * pct_


def vertex3(x, y):
    """vertex of the parabola through 3 points on ln(y); None if not concave"""
    (x0, x1, x2) = x
    try:
        l0, l1, l2 = math.log(y[0]), math.log(y[1]), math.log(y[2])
    except ValueError:
        return None
    # Lagrange form -> a, b of a x^2 + b x + c
    d01, d02, d12 = x0 - x1, x0 - x2, x1 - x2
    if abs(d01) < 1e-6 or abs(d02) < 1e-6 or abs(d12) < 1e-6:
        return None
    a = l0 / (d01 * d02) - l1 / (d01 * d12) + l2 / (d02 * d12)
    b = -l0 * (x1 + x2) / (d01 * d02) + l1 * (x0 + x2) / (d01 * d12) - l2 * (x0 + x1) / (d02 * d12)
    if a >= -1e-12:
        return None
    v = -b / (2 * a)
    return min(max(v, min(x) - 40), max(x) + 40)


def retrace(c, k, thr_pct, ref, start, d, max_steps):
    """afd_retrace: unmeasured back move to start, then measured (pair) d*FINE steps; hit at s >= thr% of ref."""
    P = c.P
    if start != c.pos:
        c.mv(start - c.pos, 0)
    pm, pi, hit = 0.0, -1, 0
    for i in range(max_steps):
        s, g = c.mv(d * P.FINE, 2)
        if s > pm:
            pm, pi = s, i
        if not below(s, ref, thr_pct):
            hit, pm, pi = 1, s, i
            break
        if i > pi and below(s, pm, P.RT_FALL):
            break
    return hit, pm, pi


def retrace_chain(c, pmax, ref, d, back1, back2):
    """retrace 1 (90%), retrace 2 (80%), else walk to the best point of retrace 2 without a threshold."""
    P = c.P
    hit, start, bidx, land = 0, 0, 0, 0.0
    for k in (1, 2):
        back = back1 if k == 1 else back2
        start = pmax - d * back
        hit, pm, bidx = retrace(c, k, P.RT_THR1 if k == 1 else P.RT_THR2, ref, start, d,
                                int(back // P.FINE) + P.RT_SLACK + (P.RT_EXTRA // P.FINE) * (k - 1))
        land = pm
        if hit:
            return True
    c.mv(start - c.pos, 0)
    for i in range(max(bidx, 0) + 1):
        c.mv(d * P.FINE, 2)
    return False


def coarse_search(c, s0, g0, predictive=False):
    """probe + coarse g climb (steps of COARSE).  Returns (bgpos, d) or None on a hard failure."""
    P = c.P
    d = 1
    bg, bgpos = g0, 0.0
    pts = [(0.0, g0)]

    def gbest():
        nonlocal bg, bgpos
        if c.last_g > bg:
            bg, bgpos = c.last_g, c.pos
        pts.append((c.pos, c.last_g))

    c.mv(P.COARSE, 1)
    cm = 1
    gc = c.last_g
    gbest()
    if below(gc, g0, P.PROBE):
        d = -1
        c.goto_move(-P.COARSE, -1)
        cm += 1
        gc = c.last_g
        gbest()
    elif not (gc * 100 > g0 * (200 - P.PROBE)):
        c.mv(P.COARSE, 1)
        cm += 1
        g2 = c.last_g
        gbest()
        if g2 * 100 > gc * 105:
            gc = g2
        else:
            d = -1
            c.goto_move(-P.COARSE, -1)
            cm += 1
            gc = c.last_g
            gbest()
    drops = 1 if below(gc, bg, P.C_DROP) else 0
    flats = 0
    prev = gc
    stop = below(gc, bg, P.C_STOP)
    if predictive:
        stop = below(gc, bg, 93) and len(pts) >= 3
    while not stop and cm < P.MAX_COARSE and c.cam.travel + P.COARSE <= P.max_travel:
        c.mv(d * P.COARSE, 1)
        cm += 1
        gc = c.last_g
        gbest()
        if predictive:
            stop = below(gc, bg, 93)
            continue
        drops = drops + 1 if below(gc, bg, P.C_DROP) else 0
        df = abs(gc - prev)
        flats = flats + 1 if df * 100 < prev * P.FLAT else 0
        prev = gc
        stop = below(gc, bg, P.C_STOP) or drops >= P.C_DROP_N or flats >= P.FLAT_N
    if predictive:
        # parabola on ln(g) through the best point and its neighbours (same-direction points only)
        srt = sorted(pts)
        ib = max(range(len(srt)), key=lambda i: srt[i][1])
        if 0 < ib < len(srt) - 1:
            v = vertex3([srt[ib - 1][0], srt[ib][0], srt[ib + 1][0]], [srt[ib - 1][1], srt[ib][1], srt[ib + 1][1]])
            if v is not None:
                bgpos = v
    return bgpos, d


def fine_and_retrace(c, bgpos, d, predictive=False, seed_pts=None):
    """pass A (FINE steps on s until two drops < 70 %) + retrace chain.  Returns 'ok' or 'nopeak'.
    seed_pts: already measured same-direction points [(pos, s)] (warm start: no back move)."""
    P = c.P
    fpos, fs = [], []
    pa_start = bgpos - d * P.COARSE
    for att in range(2):
        if seed_pts is None:
            back = pa_start - c.pos
            if abs(back) >= 1:
                c.mv(back, 0)
        n, im, below_n = 0, 0, 0
        fpos, fs = [], []
        if seed_pts:
            for (pp, ss) in seed_pts:
                fpos.append(pp)
                fs.append(ss)
                if n == 0 or ss > fs[im]:
                    im, below_n = n, 0
                n += 1
        stopped_pred = None
        while n < P.FINE_MAX and below_n < P.FINE_DROP_N:
            s, g = c.mv(d * P.FINE, 1)
            fs.append(s)
            fpos.append(c.pos)
            if n == 0 or s > fs[im]:
                im, below_n = n, 0
            elif below(s, fs[im], P.FINE_DROP):
                below_n += 1
            else:
                below_n = 0
            n += 1
            if predictive and im >= 1 and n - 1 == im + 1 and below(fs[n - 1], fs[im], 92):
                v = vertex3(fpos[im - 1:im + 2], fs[im - 1:im + 2])
                if v is not None:
                    stopped_pred = v
                    break
        if im != 0 or att == 1 or seed_pts:
            break
        pa_start -= d * 200
    fmax, fmin = fs[im], min(fs)
    pmax = fpos[im] if stopped_pred is None else stopped_pred
    ssm = n - 1 - im
    if fmax * 100 < fmin * P.NOPEAK:
        return 'nopeak'
    retrace_chain(c, pmax, fmax, d, P.RT_BACK + ssm * P.FINE, P.RT_BACK2 + ssm * P.FINE)
    return 'ok'


def algo_A(cam, P, predictive=False):
    """A (and F with predictive=True): probe + coarse g climb + fine pass A + feedback retrace."""
    c = Ctl(cam, P)
    s0, g0 = cam.measure(2)
    c.last_g = g0
    if cam.moves == 0:
        pass
    r = coarse_search(c, s0, g0, predictive)
    bgpos, d = r
    fine_and_retrace(c, bgpos, d, predictive)


def algo_F(cam, P):
    algo_A(cam, P, predictive=True)


# ---- continuous sweep ----

def sweep_move(c, d, L, s0, g0, stop_on_drop=True):
    """One long move d*L sampled every frame; pts[0] is the reading at the start.  Stops (with stop latency) once
    the s peak has fallen below 70 % twice, or early when the start reading stays the best (wrong direction: the
    peak is behind).  Returns dict(found, wrong, pk, smax, ref, floor, pts, im)."""
    P, cam = c.P, c.cam
    h = cam.begin(d * L)
    p0 = c.pos
    pts = [(p0, s0, g0)]
    smax, im, below_n = s0, 0, 0
    floor = s0
    wrong = False
    stopped = False
    while True:
        r = cam.next_sample(h)
        if r is None:
            break
        tau, s, g = r
        pts.append((p0 + cam.est_img_pos(h, tau), s, g))
        floor = min(floor, s)
        if s > smax:
            smax, im, below_n = s, len(pts) - 1, 0
        elif below(s, smax, P.FINE_DROP):
            below_n += 1
        else:
            below_n = 0
        if im == 0 and len(pts) >= 5 and below(pts[-1][1] + pts[-2][1], 2 * s0, 92) and below(pts[-1][2] + pts[-2][2], 2 * g0, 96):
            wrong = True
        if wrong or (stop_on_drop and im >= 1 and below_n >= 2 and smax >= 1.15 * floor):
            stopped = True
            c.pos += cam.stop(h)
            break
    if not stopped:
        c.pos += d * L
    found = im >= 1 and smax >= 1.15 * floor and (below_n >= 1 or im < len(pts) - 1) and not wrong
    pk, ref = None, smax
    if im >= 1:
        pk = pts[im][0]
        if im < len(pts) - 1:
            xs = [pts[im - 1][0], pts[im][0], pts[im + 1][0]]
            ys = [pts[im - 1][1], pts[im][1], pts[im + 1][1]]
            v = vertex3(xs, ys)
            if v is not None and min(xs) <= v <= max(xs):
                pk = v
                # predicted peak value from the same parabola (capped at +12 % of the best sample)
                l = [math.log(y) for y in ys]
                d01, d02, d12 = xs[0] - xs[1], xs[0] - xs[2], xs[1] - xs[2]
                if min(abs(d01), abs(d02), abs(d12)) > 1e-6:
                    a_ = l[0] / (d01 * d02) - l[1] / (d01 * d12) + l[2] / (d02 * d12)
                    b_ = -l[0] * (xs[1] + xs[2]) / (d01 * d02) + l[1] * (xs[0] + xs[2]) / (d01 * d12) - l[2] * (xs[0] + xs[1]) / (d02 * d12)
                    c_ = l[0] * xs[1] * xs[2] / (d01 * d02) - l[1] * xs[0] * xs[2] / (d01 * d12) + l[2] * xs[0] * xs[1] / (d02 * d12)
                    ref = min(math.exp(a_ * v * v + b_ * v + c_), 1.12 * smax)
    return dict(found=found, wrong=wrong, pk=pk, smax=smax, ref=max(ref, smax), floor=floor, pts=pts, im=im)


def sweep_find(c, s0, g0, L0=400, max_sweeps=6):
    """sweeps until a peak is found: continue when s is still climbing at the end, reverse on a wrong-direction call,
    zig-zag with growing length when nothing is seen.  -> (res, d) or (None, d)"""
    P = c.P
    d, L = 1, L0
    for k in range(max_sweeps):
        if c.cam.travel + L > P.max_total:
            break
        if k:
            s0, g0 = c.cam.measure(1)
        res = sweep_move(c, d, L, s0, g0)
        if res['found']:
            return res, d
        if res['wrong']:
            d = -d
        elif res['im'] >= 1 and res['smax'] >= 1.15 * res['floor']:
            pass              # climbing at the end of the sweep: go on the same way
        else:
            d, L = -d, L + L0
    return None, d


def algo_B(cam, P, warm_ok=False, back1=80, back2=160):
    """B: continuous sweep (peak from time->position), then stepped feedback retrace."""
    c = Ctl(cam, P)
    s0, g0 = cam.measure(2)
    res, d = sweep_find(c, s0, g0)
    if res is None:
        return
    retrace_chain_from_pk(c, res, d, back1, back2)


def retrace_chain_from_pk(c, res, d, back1, back2):
    retrace_chain(c, res['pk'], res['ref'], d, back1, back2)


def algo_C(cam, P, back_c=80, thr_stop=85, run_extra=100):
    """C: continuous sweep, then a continuous retrace with stop-on-threshold (stop latency applies), then a stepped
    retrace if the landing is not good."""
    c = Ctl(cam, P)
    s0, g0 = cam.measure(2)
    res, d = sweep_find(c, s0, g0)
    if res is None:
        return
    ref = res['ref']
    c.mv(res['pk'] - d * back_c - c.pos, 0)               # back move (reversal)
    h = cam.begin(d * (back_c + run_extra))
    p0 = c.pos
    hit = False
    while True:
        r = cam.next_sample(h)
        if r is None:
            c.pos += d * (back_c + run_extra)
            break
        tau, s, g = r
        if not below(s, ref, thr_stop):
            c.pos += cam.stop(h)
            hit = True
            break
    s, g = cam.measure(2)
    if below(s, ref, P.RT_THR1):
        retrace_chain(c, c.pos, ref, d, back_c + 40, back_c + 120)


def algo_D(cam, P, m=40, jump_margin=0):
    """D: model jump.  3 same-direction g samples, Gaussian fit with known width and floor, one jump to the predicted
    peak, then pass A + retrace; falls back to B when the fit finds no signal or pass A finds no peak."""
    c = Ctl(cam, P)
    s0, g0 = cam.measure(2)
    xs, ys = [], []
    for _ in range(3):
        s, g = c.mv(m, 1)
        xs.append(c.pos)
        ys.append(g)
    sig, Bk = cam.sig_cal, cam.bg_known
    best = None
    pl = xs[-1]
    p = pl - 800.0
    while p <= pl + 800.0:
        e = [math.exp(-(x - p) ** 2 / (2 * sig * sig)) for x in xs]
        see = sum(v * v for v in e)
        A = max(0.0, sum(a * (b - Bk) for a, b in zip(e, ys)) / see) if see > 1e-9 else 0.0
        sse = sum((Bk + A * a - b) ** 2 for a, b in zip(e, ys))
        if best is None or sse < best[0] - 1e-9 or (abs(sse - best[0]) <= 1e-9 and abs(p - pl) < abs(best[1] - pl)):
            best = (sse, p, A)
        p += 4.0
    ok = best is not None and best[2] / Bk >= 0.25 and ys[-1] > 1.02 * Bk
    if ok:
        pstar = best[1]
        d_ap = sgn(pstar - pl) or 1
        r = fine_and_retrace(c, pstar, d_ap)
        if r == 'ok':
            return
    s0, g0 = cam.measure(2)
    res, d = sweep_find(c, s0, g0)
    if res is not None:
        retrace_chain_from_pk(c, res, d, 80, 160)


def algo_E(cam, P):
    """E: warm start.  Local +-40 check with 20-pulse steps; a clear s slope hands over to pass A (+ retrace), a flat
    reading falls back to B."""
    c = Ctl(cam, P)
    s0, g0 = cam.measure(2)
    s1, _ = c.mv(P.FINE, 1)
    s2, _ = c.mv(P.FINE, 1)
    if s1 > 1.02 * s0 or (s1 >= 0.98 * s0 and s2 < 0.95 * s1):
        r = fine_and_retrace(c, c.pos, 1, seed_pts=[(c.pos - P.FINE, s1), (c.pos, s2)])
        if r == 'ok':
            return
    elif s1 < 0.96 * s0:
        # falling: peak behind.  Go back past the start (reversal) and climb the other way
        c.mv(-(2 * P.FINE + P.FINE), 0)
        s3, _ = c.mv(-P.FINE, 1)
        s4, _ = c.mv(-P.FINE, 1)
        r = fine_and_retrace(c, c.pos, -1, seed_pts=[(c.pos - (-P.FINE), s3), (c.pos, s4)])
        if r == 'ok':
            return
    s0, g0 = cam.measure(2)
    res, d = sweep_find(c, s0, g0)
    if res is not None:
        retrace_chain_from_pk(c, res, d, 80, 160)


ALGOS = [
    ('A', 'baseline: probe + coarse g + pass A + retrace', algo_A),
    ('B', 'continuous sweep + stepped retrace', algo_B),
    ('C', 'continuous sweep + continuous retrace (stop on threshold)', algo_C),
    ('D', 'model jump (3 g samples, known sigma) + pass A + retrace', algo_D),
    ('E', 'warm start (+-40 check) -> pass A, else B', algo_E),
    ('F', 'baseline with predictive stop (log-parabola)', algo_F),
]


# ----------------------------------------------------------------------------------------
# Part 3: Monte Carlo, report, CLI
# ----------------------------------------------------------------------------------------

CLASSES = (('near', 15, 120), ('mid', 120, 300), ('far', 300, 600), ('warm', 0, 50))
ALGO_FN = {k: f for k, d, f in ALGOS}
# B+ / C+: retrace margins scaled to the (oracle) reversal range R, to show how much of the R sensitivity is the fixed margin
ALGO_FN['B+'] = lambda cam, P: algo_B(cam, P, back1=P.R + 40, back2=P.R + 120)
ALGO_FN['C+'] = lambda cam, P: algo_C(cam, P, back_c=P.R + 40)


def calibrate(P, tm, ov, noise):
    """Params fitted from the logs (falls back to the defaults when a piece is missing)."""
    kw = {}
    if tm:
        kw.update(a_fast=tm['a'], b_fast=tm['b'], sd_fast=tm['res_sd'], slow_ms=tm['slow_ms'],
                  p_slow=tuple((e, max(0.0, min(1.0, p))) for e, n, p in tm['p_slow']))
    if ov and ov['single']:
        kw.update(gap_single=float(ov['single']), gap_pair=float(ov['pair'] or ov['single']), gap_bare=float(ov['bare'] or P.gap_bare))
    if noise:
        robust = [v[0] for k, v in noise.items() if 'MAD' in k]
        if robust:
            kw['noise'] = sum(robust) / len(robust)
    return replace(P, **kw)


def synth_curve(rng):
    ss = math.exp(rng.uniform(math.log(12), math.log(60)))
    ab = math.exp(rng.uniform(math.log(0.4), math.log(3.0)))
    return dict(sig_s=ss, ab_s=ab, sig_g=ss * rng.uniform(1.2, 2.2), ab_g=ab * rng.uniform(0.5, 1.5))


def run_trial(algo, P, spec):
    rng = random.Random(spec['seed'])
    cam = Cam(P, rng, spec['curve'], spec['x0'], spec['noise_mult'])
    timeout = False
    try:
        ALGO_FN[algo](cam, P)
    except AFTimeout:
        timeout = True
    return dict(ok=cam.quality() >= 0.9, t=min(cam.t, P.cap_ms) / 1000.0, moves=cam.moves, travel=cam.travel, timeout=timeout)


def make_specs(cls, n, seed, pool, curve_src, noise_mult):
    lo, hi = next((l, h) for c, l, h in CLASSES if c == cls)
    specs = []
    for i in range(n):
        r = random.Random('%s/%s/%d' % (seed, cls, i))
        curve = r.choice(pool) if (curve_src == 'real' and pool) else synth_curve(r)
        specs.append(dict(curve=curve, x0=r.choice((-1, 1)) * r.uniform(lo, hi), seed='%s/%s/%d/cam' % (seed, cls, i),
                          noise_mult=noise_mult))
    return specs


def run_cell(algo, P, classes, n, seed, pool, curve_src='real', noise_mult=1.0):
    res = []
    for cls in classes:
        for spec in make_specs(cls, n, seed, pool, curve_src, noise_mult):
            res.append(run_trial(algo, P, spec))
    return summarize(res)


def summarize(res):
    n = len(res)
    ts = [r['t'] for r in res]
    return dict(n=n, succ=100.0 * sum(r['ok'] for r in res) / n, t50=med(ts), t90=pct(ts, 90),
                moves=med([r['moves'] for r in res]), travel=med([r['travel'] for r in res]),
                to=100.0 * sum(r['timeout'] for r in res) / n,
                # time of the successful trials only
                t50ok=med([r['t'] for r in res if r['ok']]) if any(r['ok'] for r in res) else float('nan'))


def fmt_cell(c):
    return '%.0f%% / %.1f s / %.1f s' % (c['succ'], c['t50'], c['t90'])


def stats_row(vals):
    if not vals:
        return ['-'] * 7
    return [str(len(vals)), '%.1f' % min(vals), '%.1f' % pct(vals, 10), '%.1f' % med(vals), '%.1f' % pct(vals, 90), '%.1f' % max(vals),
            '%.1f' % statistics.mean(vals)]


def md_table(head, rows):
    out = ['| ' + ' | '.join(head) + ' |', '|' + '|'.join('---' for _ in head) + '|']
    for r in rows:
        out.append('| ' + ' | '.join(str(x) for x in r) + ' |')
    return '\n'.join(out)


def build_report(args, data, fits, pool, P, tm, ov, noise, selftest, results, elapsed):
    L = []
    w = L.append
    w('# AF replay report')
    w('')
    w('Generated by `contrib/af_replay/af_replay.py` (`--trials %d --seed %s`, %.0f s run time).  Logs: `%s`.' % (args.trials, args.seed, elapsed, args.logs))
    w('')
    w('This is a simulation. The curve shapes, noise and step timing come from the real logs; the lens model '
      '(reversal offset, sweep speed, stop latency) is assumed, and every number below that depends on those '
      'assumptions says so. See "Assumptions and their uncertainty".')
    w('')
    # ---------------- method
    w('## 1. Method')
    w('')
    w('**Real data.** Logs parsed: ' + ', '.join('%s x%d' % (k, v) for k, v in sorted(data['nfiles'].items())) +
      '. Move lines `ms phase cmd pos s g luma done_val flags done_ms ...` of AFDRIVE/AFCALIB are cut into '
      '*same-direction series*: a reversal move plus the same-direction moves after it (a reversal shifts every later '
      'point by one unknown offset, so positions inside a series are consistent, across series they are not). '
      'Series are broken at direction changes, unmeasured moves (`o`), phase-letter changes (`u`/`d` passes) and error/no-done moves.')
    w('')
    w('**Curve fit.** Each series with >= 5 points is fitted with `B + A*exp(-(p-p*)^2/(2 sigma^2))`, separately for s and g '
      '(grid search over p* and sigma, B and A by linear least squares, B limited to 0.4..1.0 x the series minimum). '
      'A fit is accepted when R^2 >= 0.85, A/B >= 0.2, sigma in 5.5..290 and p* inside the series (+-10 pulses). '
      'Coarse series (step 80-100) are too sparse to fit s; g is fitted for steps <= 100.')
    w('')
    w('**Simulator.** True lens position `x`, s peak at 0. `move(n)` adds `n` plus, when the direction differs from the previous '
      'move, a uniform random offset in [-R, +R] (R = %g pulses by default). Same-direction moves are exact. FOCUS_DONE time: '
      'fast mode `%.0f + %.2f*|n|` ms (sd %.0f) or, with probability by size, the ~%.0f ms timer mode seen in the logs; '
      'after FOCUS_DONE %.0f ms (unmeasured) / %.0f ms (+ one sample) / %.0f ms (+ stable pair), fitted from the log timestamps. '
      'Readings carry multiplicative Gaussian noise (sd %.1f %% per reading, 1/sqrt(2) for a pair). Curves are drawn from the accepted real fits '
      '(joint s and g) or synthetic ones (sigma_s 12-60, A/B 0.4-3). g additionally has a wide component (%.0f %% weight, sigma %.0f) '
      'because the brief says g is broad (+-120) although the fitted g cores are narrow.'
      % (P.R, P.a_fast, P.b_fast, P.sd_fast, P.slow_ms, P.gap_bare, P.gap_single, P.gap_pair, 100 * P.noise, 100 * P.g_wide_w, P.g_wide_sig))
    w('')
    w('Continuous moves (`begin`/`next_sample`/`stop`): one LV frame every %.0f ms, constant speed %.2f pulses/ms after a %.0f ms start '
      'delay, the frame shows the lens position one frame (%.0f ms) earlier, the algorithm converts time to position with its own '
      'velocity estimate (error sd %.0f %%) and the same lag. `stop()` lets the lens coast for the stop latency (default %.0f ms).'
      % (P.frame, P.sweep_v, P.start_delay, P.lag_true, 100 * P.vel_err, P.stop_lat))
    w('')
    w('**Algorithms** (all over the same camera API, same constants as `afd_run` where they overlap):')
    w('')
    for k, d, f in ALGOS:
        w('- **%s** - %s' % (k, d))
    w('')
    w('- A mirrors `afd_run`: probe step 100 on g (95 % rule, ambiguous band), coarse g climb (stop at < 80 % once or < 90 % twice or flat), '
      'pass A (20-pulse steps on s from one coarse step before the g peak, stop after 2 points < 70 % of max, widen by 200 once when the max is the first point, '
      'no-peak check at 115 %), retrace 1 (back 60 + steps-since-max x 20, stable pairs, stop at s >= 90 % of the pass A max), '
      'retrace 2 (80 %, back 140+), then walk to the best point. Run cap 15 s.')
    w('- B/C sweep: one 400-pulse move, one sample per frame, stop when the s peak (>= 1.15 x floor) has dropped below 70 % twice or when the start reading stays '
      'the best (peak behind), peak = log-parabola vertex of the 3 best frames, zig-zag with growing length if nothing is seen. '
      'B retraces stepwise (back 80 / 160), C continuously: back move to peak - 80, a continuous move with stop at s >= 85 % of the sweep peak, then the '
      'stepped retrace if the landing is below 90 %.')
    w('- D: 3 same-direction 40-pulse moves with one g sample each; Gaussian on g with the calibrated width (+-10 %) and the floor known to +-15 %; '
      'amplitude and p* fitted; no signal (A/B < 0.25) or no peak in pass A -> B.')
    w('- E: s at the start and after +20, +20; rising / peak at the middle point -> pass A + retrace without coarse search; falling -> the same the other way; flat -> B.')
    w('- F: A with predictive stop: coarse g climb stops at the first drop below 93 % of the best and uses the log-parabola vertex as the g peak; '
      'pass A stops after the first drop below 92 % when the log-parabola through the 3 points is concave and uses its vertex as the peak.')
    w('')
    w('**Metrics.** Over N random trials per cell: success = final true s >= 90 %% of the true peak s (noise-free, at the final true lens position); '
      'time = simulated ms from the start until the algorithm ends (capped at 15 s, counted as is); moves = FOCUS_DONE count; travel = sum of |pulses| commanded. '
      'Start defocus is uniform in |x0| and a random sign: near %d-%d, mid %d-%d, far %d-%d, warm %d-%d pulses. '
      'All algorithms see the same curves, start positions and per-trial random seeds.' % (CLASSES[0][1], CLASSES[0][2], CLASSES[1][1], CLASSES[1][2], CLASSES[2][1], CLASSES[2][2], CLASSES[3][1], CLASSES[3][2]))
    w('')
    # ---------------- fitted stats
    w('## 2. What the logs say')
    w('')
    acc_s = [x for x in fits if x['ok_s'] and x['kind'] != 'MPUDRIVE']
    acc_g = [x for x in fits if x['ok_g'] and x['kind'] != 'MPUDRIVE']
    nser = len([x for x in fits if x['kind'] != 'MPUDRIVE'])
    w('%d same-direction series with >= 5 points; accepted fits: s %d, g %d, both %d (curve pool for the simulator: %d).' % (
        nser, len(acc_s), len(acc_g), len([x for x in fits if x['ok_s'] and x['ok_g'] and x['kind'] != 'MPUDRIVE']), len(pool)))
    w('')
    rows = []
    for label, sel in (('all accepted', lambda x: True), ('AFCALIB passes (step 40)', lambda x: x['kind'] == 'AFCALIB'),
                       ('AFDRIVE passes (step 10-20)', lambda x: x['kind'] == 'AFDRIVE'), ('AFSWEEP', lambda x: x['kind'] == 'AFSWEEP')):
        for nm, key, okk in (('sigma_s (pulses)', 'fs', 'ok_s'), ('A/B  s', 'fs', 'ok_s'), ('sigma_g (pulses)', 'fg', 'ok_g'), ('A/B  g', 'fg', 'ok_g')):
            fld = 'sig' if 'sigma' in nm else 'ab'
            vals = [x[key][fld] for x in fits if x[okk] and x['kind'] != 'MPUDRIVE' and sel(x)]
            if vals:
                rows.append([label, nm] + stats_row(vals))
    w(md_table(['source', 'quantity', 'n', 'min', 'p10', 'median', 'p90', 'max', 'mean'], rows))
    w('')
    jr = [x['fg']['sig'] / x['fs']['sig'] for x in fits if x['ok_s'] and x['ok_g'] and x['kind'] != 'MPUDRIVE']
    if jr:
        w('sigma_g / sigma_s where both fit: median %.2f (p10 %.2f, p90 %.2f). FWHM of s = 2.35 sigma_s = median %.0f pulses, i.e. the brief\'s 40-60 pulses at x5.' % (
            med(jr), pct(jr, 10), pct(jr, 90), 2.355 * med([x['fs']['sig'] for x in acc_s])))
    w('')
    w('Remark: the fitted g cores are *narrow* (sigma %.0f median). The coarse 60/100-pulse logs show g returning to its floor one step (60-100 pulses) from the peak in most scenes, but a '
      'few runs (card7) show g and s clearly broader. The "g is about +-120 broad" statement is therefore modelled as an extra wide component, not as a single Gaussian; '
      'it only matters for the probe/coarse phase and for D.' % med([x['fg']['sig'] for x in acc_g] or [0]))
    w('')
    bad = [x for x in fits if x['kind'] != 'MPUDRIVE' and not x['ok_s'] and x['fs'] is not None]
    w('Rejected s fits: %d (mostly retrace series that end at the 90 %% threshold, so the peak is outside or at the edge, plus a few low-contrast or noisy ones).' % len(bad))
    w('')
    mp = [x for x in fits if x['kind'] == 'MPUDRIVE']
    if mp:
        w('MPUDRIVE speed-07 steps (+-100/+-300) are already continuous sweeps. With pos = cmd*(t-lag)/T_done their s fits give sigma_s = ' +
          ', '.join('%.0f' % x['fs']['sig'] for x in mp if x['fs']) + ' pulses (and wildly different values for 100 vs 300 pulse moves), '
          'i.e. the time->position map is not trustworthy for them: FOCUS_DONE of long moves is the ~510 ms timer value, not the motion end. '
          'They are therefore excluded from the pool. The AFSWEEP log (positions from time, but with the real done time) is the proper source for sweep geometry.')
        w('')
    w('### AFSWEEP.LOG')
    w('')
    sw = [x for x in fits if x['kind'] == 'AFSWEEP']
    if data['sweeps']:
        w('Found %d AFSWEEP file(s), %d fitted sweep series.' % (len(data['sweeps']), len(sw)))
    else:
        w('No AFSWEEP.LOG in the supplied logs. The parser is implemented (`parse_afsweep`, `sweep_series`) and checked on a synthetic sample: ' +
          ('**self-test passed**' if selftest[0] else '**self-test FAILED**') + ' (' + '; '.join('cmd %d: %d samples, s fit p*=%s sigma=%s' % (c, n, (fs or ('-', '-'))[0], (fs or ('-', '-'))[1]) for c, n, fs, fg, ok in selftest[1]) +
          '; sample peak 300 / -250, sigma 25). Format: `# AFSWEEP ...`, `# sweep k= cmd= speed= dir= [t=]`, `S t_ms frame s1 s2 s4 g q band luma`, `# done k= t= val= n=`; '
          'pos = cmd*(t - t_start - lag)/(t_done - t_start), t_start = the sweep header t= or else the first S line, lag default 42 ms. Drop the file in the logs folder and rerun: '
          'its series join the fit statistics and the curve pool automatically.')
    w('')
    w('### Move timing and overhead')
    w('')
    if tm:
        w('Fitted from %d valid moves (flags clean, speed 0x07; timer-mode = done_val bit 0x0400, FOCUS_DONE ~%.0f ms whatever the size):' % (tm['n'], tm['slow_ms']))
        w('')
        w('- fast mode (%d moves): `t = %.0f + %.2f*|n|` ms, residual sd %.0f ms' % (tm['fast_n'], tm['a'], tm['b'], tm['res_sd']))
        w('- single line over all moves, as asked: `t = %.0f + %.2f*|n|` ms; with a reversal term: `%.0f + %.2f*|n| + %.0f*reversed`' % (tm['a0'], tm['b0'], tm['all_with_rev'][0], tm['all_with_rev'][1], tm['all_with_rev'][2]))
        w('- share of timer-mode completions by |n|: ' + ', '.join('<=%s: %.0f %% (n=%d)' % ('inf' if e >= 100000 else e, 100 * p, nn) for e, nn, p in tm['p_slow']))
        w('- so 20-40 pulses take ~140-250 ms and 120+ ~350-550 ms, as in the brief; the exact split between a real motion time and the 510 ms timer is unknown.')
        w('- per-move overhead after FOCUS_DONE (median of log timestamp gaps): unmeasured %.0f ms, + one sample %.0f ms (n=%d), + stable pair %.0f ms (n=%d).' % (ov['bare'] or 0, ov['single'] or 0, ov['n1'], ov['pair'] or 0, ov['n2']))
        w('')
    w('### Noise')
    w('')
    for k, (v, n) in sorted(noise.items()):
        w('- %s: %.2f %% relative sd per reading (n=%d)' % (k, 100 * v, n))
    w('')
    w('The sd-based estimates are inflated by a few LV-recovery outliers right after the monitor open; the MAD-based ones describe the normal scene. '
      'The simulator uses the mean of the MAD estimates (%.2f %%) as "fitted" noise and 3x that as "high noise".' % (100 * P.noise))
    w('')
    # ---------------- results
    R = results
    w('## 3. Results')
    w('')
    w('Cells are `success / median time / p90 time`; N = %d trials per class. Defaults: R = %g, stop latency %g ms, sweep speed %.2f pulses/ms, real curves, fitted noise. '
      'Success = final s >= 90 %% of the true peak.' % (args.trials, P.R, P.stop_lat, P.sweep_v))
    w('')
    names = [k for k, d, f in ALGOS]
    rows = []
    for k in names:
        row = [k]
        for cls, lo, hi in CLASSES:
            row.append(fmt_cell(R['main'][(k, cls)]))
        row.append(fmt_cell(R['cold'][k]))
        rows.append(row)
    w('**Start defocus (real curves, fitted noise).** "cold" = near + mid + far pooled.')
    w('')
    w(md_table(['algo'] + ['%s (%d-%d)' % c for c in CLASSES] + ['cold'], rows))
    w('')
    rows = []
    for k in names:
        c = R['cold'][k]
        rows.append([k, '%.0f%%' % c['succ'], '%.1f' % c['t50'], '%.1f' % c['t90'], '%.1f' % c['t50ok'], '%.0f' % c['moves'], '%.0f' % c['travel'], '%.1f%%' % c['to']])
    w('**Cold pooled, all metrics.**')
    w('')
    w(md_table(['algo', 'success', 'median s', 'p90 s', 'median s (successes)', 'moves', 'travel (pulses)', 'timeouts'], rows))
    w('')
    w('**Noise and curve family (near + mid pooled).**')
    w('')
    rows = []
    for k in names:
        rows.append([k] + [fmt_cell(R['var'][(k, v)]) for v in ('real/1x', 'real/3x', 'synth/1x', 'synth/3x')])
    w(md_table(['algo', 'real curves, noise 1x', 'real, 3x', 'synthetic curves, 1x', 'synthetic, 3x'], rows))
    w('')
    w('**Sensitivity to the reversal offset range R** (near + mid pooled; algorithm constants were designed for R ~ 60).')
    w('')
    rows = [[k] + [fmt_cell(R['R'][(k, r)]) for r in R['R_list']] for k in names + ['B+', 'C+']]
    w(md_table(['algo'] + ['R = %g' % r for r in R['R_list']], rows))
    w('')
    w('B+ / C+ = B / C with the retrace margin set to R + 40 (second retrace R + 120) instead of the fixed 80 / 160, using the true R (an oracle): this shows how much of the loss at large R is the fixed margin, which a calibration of the reversal range could fix.')
    w('')
    w('**Sensitivity to the stop latency** (sweep algorithms; near + mid pooled; A, D, E, F never call stop()).')
    w('')
    rows = [[k] + [fmt_cell(R['lat'][(k, l)]) for l in R['lat_list']] for k in ('B', 'C')]
    w(md_table(['algo'] + ['stop %g ms' % l for l in R['lat_list']], rows))
    w('')
    w('**Sensitivity to the sweep speed** (not measured, AFSWEEP.LOG will give it; B, C, E, D use sweeps as fallback).')
    w('')
    rows = [[k] + [fmt_cell(R['v'][(k, v)]) for v in R['v_list']] for k in ('B', 'C', 'D', 'E')]
    w(md_table(['algo'] + ['%.2f pulses/ms' % v for v in R['v_list']], rows))
    w('')
    w('**Sensitivity to a wrong image-lag assumption** (the sweep algorithms believe the lag is 42 ms; true lag = 42 + error).')
    w('')
    rows = [[k] + [fmt_cell(R['lag'][(k, e)]) for e in R['lag_list']] for k in ('B', 'C')]
    w(md_table(['algo'] + ['lag error %+g ms' % e for e in R['lag_list']], rows))
    w('')
    # ---------------- recommendation
    w('## 4. Recommendation')
    w('')
    w(recommendation_text(R, names, P))
    w('')
    w('## 5. Assumptions and their uncertainty')
    w('')
    w(ASSUMPTIONS)
    w('')
    return '\n'.join(L)


ASSUMPTIONS = '''1. **Reversal offset: uniform in [-R, +R], R = 60, redrawn at each reversal, same-direction moves exact.** Only loosely supported
   (calib backlash_est values were -120..+120 with both signs, `repeat_up` 80). Real offsets might depend on the move size or be correlated between consecutive reversals.
   The R table shows how strongly the results depend on it; the algorithms keep their constants (designed for ~60), so a larger R hurts the 20-pulse retrace the most.
2. **Sweep speed 0.40 pulses/ms is a guess.** The logs only give FOCUS_DONE, which for moves >= 60 pulses is mostly the 510 ms timer value. The speed table shows the effect; this is the first thing AFSWEEP.LOG will settle.
   Continuous motion is modelled as constant speed after a 30 ms start delay, no acceleration profile and no motion blur of s.
3. **Stop latency** is a parameter (30/60/120 ms), the coast distance is latency x speed. Whether `stop()` (FOCUS_STOP, (9,3,1)) halts a (9,1) move at all is untested.
4. **Curve shape.** A single Gaussian on a flat floor for s (fits R^2 mostly > 0.95 in the stepped windows); the real tails are not modelled and the pool has only %POOL% curves from a handful of scenes
   (same lens, x5, a few distances), so the scene variety (sigma 17-30 pulses) is probably too narrow; the synthetic family widens it to 12-60. g has an assumed wide component.
   The floor is a constant (no scene gradient), so false peaks and texture changes are not simulated.
5. **Noise** is multiplicative Gaussian, ~1 % per reading from static repeats, independent between readings. Real noise has outliers (flicker, LV recovery) and motion blur in continuous moves is ignored.
6. **Positions** are only known to the algorithms through commanded pulses; the lens range / end stops and Canon's own AF interplay are not modelled (travel is unbounded).
7. **Timing** of stepped moves comes from the logs (fast mode / timer mode mixture, fitted overheads); the timer-mode share depends on size only in the model.
8. Algorithms D/E/F are first designs, not tuned: thresholds (92/93 %, 2 %, 0.25) were set once, not optimised. The comparison says which *structure* is promising, not the best constants.
9. Success counts only the final true s >= 90 % of the true peak. The algorithms' own landing test is 90 % of an *observed* maximum (a noisy, frame-sampled one for B/C), so some failures are the landing criterion rather than a miss of the peak.'''


def recommendation_text(R, names, P):
    cold, M = R['cold'], R['main']
    f = lambda c: '%.0f %% / %.1f s' % (c['succ'], c['t50'])
    out = []
    out.append('Headline (cold start = near + mid + far, defaults, real curves): ' + '; '.join('**%s** %s (p90 %.1f s)' % (k, f(cold[k]), cold[k]['t90']) for k in names) +
               '. Warm (|defocus| <= 50): ' + '; '.join('%s %s' % (k, f(M[(k, 'warm')])) for k in names) + '.')
    out.append('')
    out.append('1. **The stepped baseline (A) is slow because it walks, not because it is inaccurate.** Its simulated time (median %.1f s, %.0f moves cold; %.1f s near, %.1f s mid) matches the field runs (8.5-10.6 s), '
               'and it hits the 15 s cap in %.0f %% of the cold trials (far start: the coarse climb is limited to 1500 pulses and needs the g slope). '
               'The two sweep algorithms are roughly twice as fast (B %.1f s, C %.1f s median, 7-9 moves) and at least as often in focus.' % (
                   cold['A']['t50'], cold['A']['moves'], M[('A', 'near')]['t50'], M[('A', 'mid')]['t50'], cold['A']['to'], cold['B']['t50'], cold['C']['t50']))
    out.append('2. **Recommended direction: a continuous sweep to find the peak (B), finished by the existing stepped feedback retrace, with C (continuous retrace + stop on threshold) as an optional upgrade.** '
               'C buys %+.0f points of success for %+.1f s over B at the default 60 ms stop latency, but it is the more fragile one: its success falls from %.0f %% (stop latency 0) to %.0f %% (200 ms), '
               'and both lose when the real sweep speed is high (%.0f %% / %.0f %% at 0.8 pulses/ms vs %.0f %% / %.0f %% at 0.4). The stepped retrace keeps the camera\'s proven landing rule, so B degrades more gracefully. '
               'Build B first, measure the stop latency and sweep speed with AFSWEEP.LOG, then decide on C.' % (
                   cold['C']['succ'] - cold['B']['succ'], cold['C']['t50'] - cold['B']['t50'], R['lat'][('C', 0)]['succ'], R['lat'][('C', 200)]['succ'],
                   R['v'][('B', 0.8)]['succ'], R['v'][('C', 0.8)]['succ'], R['v'][('B', 0.4)]['succ'], R['v'][('C', 0.4)]['succ']))
    out.append('3. **Reversal offset is the dominant risk for every algorithm.** Going from R = 0 to R = 60 costs A %.0f, B %.0f, C %.0f and F %.0f points; at R = 100 A falls to %.0f %%, B to %.0f %%, C to %.0f %% and at R = 150 to %.0f / %.0f / %.0f %%. '
               'With the retrace margin matched to R (B+/C+, oracle) B and C recover only part of it (%.0f %% / %.0f %% at R = 100), (the remaining loss is not analysed here); '
               'a measured R (the repeated-reversal calibration the next field test should run) is worth more than any algorithm choice here.' % (
                   R['R'][('A', 0)]['succ'] - R['R'][('A', 60)]['succ'], R['R'][('B', 0)]['succ'] - R['R'][('B', 60)]['succ'], R['R'][('C', 0)]['succ'] - R['R'][('C', 60)]['succ'], R['R'][('F', 0)]['succ'] - R['R'][('F', 60)]['succ'],
                   R['R'][('A', 100)]['succ'], R['R'][('B', 100)]['succ'], R['R'][('C', 100)]['succ'], R['R'][('A', 150)]['succ'], R['R'][('B', 150)]['succ'], R['R'][('C', 150)]['succ'],
                   R['R'][('B+', 100)]['succ'], R['R'][('C+', 100)]['succ']))
    out.append('4. **Cheap, low-risk improvement of the current code: F (predictive stop in the stepped climb).** It cuts %.1f s (%.1f -> %.1f s median, cold) and gains %+.0f points; the gain is largest when the reversal offset is small (R = 0: %.0f %% vs %.0f %%). '
               'It needs no new firmware command, so it can ship before the sweep work.' % (
                   cold['A']['t50'] - cold['F']['t50'], cold['A']['t50'], cold['F']['t50'], cold['F']['succ'] - cold['A']['succ'], R['R'][('F', 0)]['succ'], R['R'][('A', 0)]['succ']))
    out.append('5. **Not recommended as designed:** D (model jump, %s cold) pays for three probe moves and then still needs pass A + retrace; it also depends on knowing sigma_g and the floor. '
               'E (warm start, %s warm) is beaten by simply running B or C even for small defocus (%s and %s warm), because the +-40 check is ambiguous near the peak and then falls back to B.' % (
                   f(cold['D']), f(M[('E', 'warm')]), f(M[('B', 'warm')]), f(M[('C', 'warm')])))
    out.append('6. **Next measurements that would change these conclusions:** AFSWEEP.LOG (sweep speed and the real FOCUS_DONE time, which fixes time->position and the peak width in continuous motion), '
               'a stop() test on a running (9,1) move (latency, coast distance), and a reversal test with many repeats at several move sizes (distribution and size dependence of the offset). '
               'Then rerun this tool: `python3 af_replay.py --logs <dir>`; AFSWEEP series are picked up automatically.')
    out.append('')
    out.append('(Text generated from the tables above; the qualitative reading assumes the default seed and N >= 300. Differences of a few points are inside the Monte Carlo noise: with N = 300 per class the 1-sigma error of a success rate is about 1.5-2 points.)')
    return '\n'.join(out)


def run_all(args, P, pool):
    R = dict(main={}, cold={}, var={}, R={}, lat={}, v={}, lag={})
    n = args.trials
    seed = args.seed
    names = [k for k, d, f in ALGOS]
    for k in names:
        pooled = []
        for cls, lo, hi in CLASSES:
            specs = make_specs(cls, n, seed, pool, 'real', 1.0)
            res = [run_trial(k, P, sp) for sp in specs]
            R['main'][(k, cls)] = summarize(res)
            if cls != 'warm':
                pooled += res
        R['cold'][k] = summarize(pooled)
    nm = n
    typ = ('near', 'mid')
    for k in names:
        for cs in ('real', 'synth'):
            for nmul in (1.0, 3.0):
                R['var'][(k, '%s/%gx' % (cs, nmul))] = run_cell(k, P, typ, nm, seed, pool, cs, nmul)
    R['R_list'] = [0, 30, 60, 100, 150]
    for k in names:
        for r in R['R_list']:
            R['R'][(k, r)] = run_cell(k, replace(P, R=float(r)), typ, nm, seed, pool)
    for k in ('B+', 'C+'):
        for r in R['R_list']:
            R['R'][(k, r)] = run_cell(k, replace(P, R=float(r)), typ, nm, seed, pool)
    R['lat_list'] = [0, 30, 60, 120, 200]
    for k in ('B', 'C'):
        for l in R['lat_list']:
            R['lat'][(k, l)] = run_cell(k, replace(P, stop_lat=float(l)), typ, nm, seed, pool)
    R['v_list'] = [0.2, 0.4, 0.8]
    for k in ('B', 'C', 'D', 'E'):
        for v in R['v_list']:
            R['v'][(k, v)] = run_cell(k, replace(P, sweep_v=v), typ, nm, seed, pool)
    R['lag_list'] = [-42, 0, 42]
    for k in ('B', 'C'):
        for e in R['lag_list']:
            R['lag'][(k, e)] = run_cell(k, replace(P, lag_err=-float(e)), typ, nm, seed, pool)
    return R


def main(argv=None):
    ap = argparse.ArgumentParser(description='AF algorithm replay/simulation against real focus curves')
    ap.add_argument('--logs', default=None, help='folder with card*/ subfolders holding AFDRIVE.LOG, AFCALIB.LOG, ... (searched recursively)')
    ap.add_argument('--trials', type=int, default=300, help='trials per defocus class (default 300)')
    ap.add_argument('--report', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'REPORT.md'))
    ap.add_argument('--seed', default='af1')
    ap.add_argument('--selftest', action='store_true', help='only run the AFSWEEP parser self-test')
    args = ap.parse_args(argv)
    st = selftest_parser(verbose=True)
    print('AFSWEEP parser self-test: %s' % ('ok' if st[0] else 'FAILED'))
    if args.selftest:
        return 0 if st[0] else 1
    if not args.logs:
        ap.error('--logs is required')
    t0 = time.time()
    data = load_logs(args.logs)
    if not data['runs']:
        print('no AFDRIVE/AFCALIB logs found under %s' % args.logs)
        return 2
    fits = fit_all_series(data)
    pool = curve_pool(fits)
    tm = solve_move_time(data['runs'])
    ov = solve_overheads(data['runs'])
    noise = solve_noise(data['runs'], data['blocks'])
    P = calibrate(Params(), tm, ov, noise)
    print('logs: %d runs, %d series, curve pool %d; noise %.2f %%; move time fast %.0f+%.2f|n|' % (
        len(data['runs']), len([x for x in fits if x['kind'] != 'MPUDRIVE']), len(pool), 100 * P.noise, P.a_fast, P.b_fast))
    if not pool:
        print('no usable curves; using synthetic ones only')
    R = run_all(args, P, pool)
    elapsed = time.time() - t0
    rep = build_report(args, data, fits, pool, P, tm, ov, noise, st, R, elapsed)
    rep = rep.replace('%POOL%', str(len(pool)))
    with open(args.report, 'w') as f:
        f.write(rep)
    print('wrote %s (%.1f s)' % (args.report, elapsed))
    for k, d, fn in ALGOS:
        c = R['cold'][k]
        print('  %s cold: success %.0f%%  median %.1f s  p90 %.1f s  moves %.0f  travel %.0f' % (k, c['succ'], c['t50'], c['t90'], c['moves'], c['travel']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
