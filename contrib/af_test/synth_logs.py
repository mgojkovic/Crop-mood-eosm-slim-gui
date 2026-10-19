#!/usr/bin/env python3
"""Synthetic AF logs with known ground truth, for the offline test suite.

The EOS M AF tests write their evidence to the card as AFDRIVE.LOG, AFBENCH.LOG,
AFRTEST.LOG, AFSWEEP.LOG and AFSEARCH.LOG.  No camera and no card are available to
the host test suite, so this script fabricates a small but realistic session from a
known scene model:

  * AFDRIVE.LOG - AF runs with `# RESULT` lines, per-move trace lines in the exact
    `ms phase cmd pos s g luma done_val flags done_ms ret dist=...` format of
    crop_rec.c, `# focal_mm=`, `# s0=` noise pairs, and `# aref trig/done`,
    `# lv on/off`, `# monitor` notes.
  * AFBENCH.LOG - `# BENCH` rows, `# BENCH_SUMMARY` lines.
  * AFRTEST.LOG - `# RTEST_BEGIN`, `# RT_HOLD1`, `# RT_TRIAL` lines and the
    `# RTEST false_trig=... ok=k/n` summary.
  * AFSWEEP.LOG - the *shipped* firmware format (`# sweep k= cmd= speed= dir=` plus
    `S <t_ms> <s1> <g> <luma>` samples, `# done k= t= val= n=`), with a known
    Gaussian peak and width.
  * AFSEARCH.LOG - a static noise block (`# ms s g ...`).

`expected.json` records the ground truth (scene peaks/sigmas, expected report
verdicts, run counts) so tests and the field-log corpus can check a replay against
the values the generator put in.  Deterministic: the same --seed produces the same
bytes.

  python3 contrib/af_test/synth_logs.py --out /tmp/af_synth [--seed N] [--profile healthy]

Profiles:

  healthy  every AF run lands, Refocus test 5/5, false_trig 0 -> "AF drive OK"
  slow     5/8 runs land, median run > 5 s                    -> "AF drive NOT OK"
  hunting  Refocus test 2/5, false_trig 2                     -> "HUNTING"

Python 3 standard library only.
"""
import argparse
import datetime
import json
import math
import os
import random
import sys

B_S = 20000.0     # s blur floor (same cosmetic scale as af_replay: only ratios matter)
B_G = 25000.0     # g blur floor
MOVE_NOISE = 0.010

# Known scenes.  "peak" is the true lens position (relative pulses) of maximum
# sharpness; AFDRIVE positions below are absolute so the fit can recover the peak.
SCENES = (
    dict(name='page', peak=300.0, sigma_s=21.0, ab_s=1.50, sigma_g=35.0, ab_g=1.20, focal=18, g_off=8.0),
    dict(name='books', peak=-250.0, sigma_s=18.0, ab_s=1.25, sigma_g=42.0, ab_g=1.05, focal=35, g_off=-5.0),
    dict(name='wall', peak=120.0, sigma_s=28.0, ab_s=0.95, sigma_g=70.0, ab_g=0.80, focal=50, g_off=12.0),
)

PROFILES = {
    # nruns, nok, (total_ms lo, hi), (ok, ntrial, false_trig)
    'healthy': dict(nruns=6, nok=6, total_ms=(3200, 4600), rtest=(5, 5, 0)),
    'slow': dict(nruns=8, nok=5, total_ms=(6000, 8000), rtest=(5, 5, 0)),
    'hunting': dict(nruns=6, nok=6, total_ms=(3200, 4600), rtest=(2, 5, 2)),
}

AF_LOGS = ('AFDRIVE.LOG', 'AFBENCH.LOG', 'AFRTEST.LOG', 'AFSWEEP.LOG', 'AFSEARCH.LOG')

# firmware FOCUS_DONE values used in the logs: bit 0x0400 is the ~510 ms timer mode
DONE_FAST = '4e0000'
DONE_SLOW = '4e0400'


def _s_at(scene, x):
    return B_S * (1.0 + scene['ab_s'] * math.exp(-(x - scene['peak']) ** 2 / (2.0 * scene['sigma_s'] ** 2)))


def _g_at(scene, x):
    u = x - scene['peak'] - scene['g_off']
    return B_G * (1.0 + scene['ab_g'] * math.exp(-u * u / (2.0 * scene['sigma_g'] ** 2)))


def _read(rng, scene, x, noise):
    return (max(1, int(round(_s_at(scene, x) * (1.0 + rng.gauss(0.0, noise))))),
            max(1, int(round(_g_at(scene, x) * (1.0 + rng.gauss(0.0, noise))))),
            int(round(112 + rng.gauss(0.0, 9))))


def _move_line(ms, ph, cmd, pos, s, g, luma, done_val, flags, done_ms, dist):
    """One crop_rec.c afd_move_ex() trace line.  The exact parser pattern is
    `ms phase cmd pos s g luma done_val flags done_ms ret<r> dist=<d> [extra...]`."""
    return ('%d %s %d %d %d %d %d %s %s %d ret0 dist=%d t=%d wait=0 samp=%d q=%d'
            % (ms, ph, cmd, pos, s, g, luma, done_val, flags, done_ms, abs(dist), ms, 165,
               (s * 1000) // max(g, 1)))


def _done_ms(rng, n, fast=True):
    if fast:
        return max(40, int(round(rng.gauss(178.0 + 0.72 * abs(n), 18.0))))
    return int(round(rng.gauss(510.0, 12.0)))


def _move(ms, ph, cmd, pos, scene, rng, noise, fast=True, dist=0):
    s, g, luma = _read(rng, scene, pos, noise)
    return _move_line(ms, ph, cmd, pos, s, g, luma, DONE_FAST if fast else DONE_SLOW, '-',
                      _done_ms(rng, cmd, fast=fast), dist if dist else abs(cmd))


def _pass(lines, t, scene, rng, noise, start, step, count, ph, gap=190.0):
    """A same-direction, fixed-step measured pass (the replay's unit of curve fit).
    Returns the updated clock.  `ms` of a move is its FOCUS_DONE time; the next move
    starts gap ms after FOCUS_DONE (same convention as solve_overheads())."""
    pos = start
    for _ in range(count):
        done = _done_ms(rng, step)
        t += done + gap
        lines.append(_move(t, ph, step, pos, scene, rng, noise, fast=True, dist=step))
        pos += step
    return t


def _noise_pair(lines, t, scene, rng, noise, pos):
    """An 'i' baseline line plus the `# s0=` comment that solve_noise() uses."""
    s0 = int(round(_s_at(scene, pos)))
    si = max(1, int(round(s0 * (1.0 + rng.gauss(0.0, noise)))))
    g = max(1, int(round(_g_at(scene, pos))))
    t += 150
    lines.append(_move_line(t, 'i', 0, pos, si, g, 118, '0', '-', 0, 0))
    lines.append('# s0=%d g=%d luma=%d' % (s0, g, 118))
    return t


def _run_header(lines, t, scene, algo='B'):
    lines.append('# AFDRIVE t=%d lens="EF-M 18-55mm F3.5-5.6 IS STM" zoom=x5 crop_preset=0 '
                 'dist0=%d monitor=KEEP opened_now=%d algo=%s' % (t, int(abs(scene['peak']) + 500), 1 if t < 3000 else 0, algo))
    lines.append('# focal_mm=%d fine=20 fine_setting=2' % scene['focal'])
    lines.append('# scene=%s peak=%d sigma=%d' % (scene['name'], int(scene['peak']), int(scene['sigma_s'])))


def _write_afdrive(path, rng, profile):
    p = PROFILES[profile]
    lines, t = [], 1000
    runs = p['nruns']
    totals = []
    for i in range(runs):
        scene = SCENES[i % len(SCENES)]
        ok = i < p['nok']
        land_lo, land_hi = (92, 99) if ok else (0, 20)
        land_pct = int(round(rng.uniform(land_lo, land_hi)))
        total = int(round(rng.uniform(*p['total_ms'])))
        totals.append(total)
        _run_header(lines, t, scene, algo=['B', 'A', 'F', 'D'][i % 4])
        if i == 0:
            for k in range(4):     # static 'i' repeats -> solve_noise() i-pairs estimate
                t = _noise_pair(lines, t, scene, rng, MOVE_NOISE, scene['peak'] - 200 + 10 * k)
        # an up pass and a down pass over the same curve: two same-direction series.
        # The step varies with the scene (and the tail below) so the fitted move-time
        # model has more than one |n| value to regress on.
        step = 20 if i % 2 == 0 else 10
        t = _pass(lines, t, scene, rng, MOVE_NOISE, scene['peak'] - 7 * step, step, 15, 'u', gap=190.0)
        t = _pass(lines, t, scene, rng, MOVE_NOISE, scene['peak'] + 7 * step, -step, 15, 'd', gap=190.0)
        # three measured moves of different sizes: they close the series and feed solve_move_time
        for tail in (40, 20, 10):
            done = _done_ms(rng, tail)
            t += done + 165
            s, g, luma = _read(rng, scene, scene['peak'] + 40, MOVE_NOISE)
            lines.append(_move_line(t, 'p', tail, scene['peak'] + 40, s, g, luma, DONE_FAST, '-', done, tail))
        # two unmeasured timer-mode moves (exercise solve_move_time, break series)
        for cmd in (300, -300):
            done = _done_ms(rng, cmd, fast=False)
            t += done + 86
            s, g, luma = _read(rng, scene, scene['peak'] + rng.uniform(-600, 600), MOVE_NOISE)
            lines.append(_move_line(t, 'o', cmd, scene['peak'] + int(rng.uniform(-500, 500)), 0, 0, luma,
                                    DONE_SLOW, '-', done, abs(cmd)))
        ab = '0' if ok else 'lowtexture'
        s_peak = int(_s_at(scene, scene['peak']))
        s_land = max(1, s_peak * land_pct // 100) if ok else max(1, s_peak // 20)
        lines.append('# RESULT aborted=%s best_pos=%d best_s=%d final_s=%d final_best_pct=%d retraces=2 '
                     'land_s=%d land_pct=%d moves=%d n_nodone=0 n_err=0 n_fast=%d total_ms=%d end_pos=%d '
                     'travel=%d algo=%s'
                     % (ab, int(scene['peak']), s_peak, s_land, land_pct, s_land, land_pct, 32,
                        30 if ok else 31, total, int(scene['peak']), 900, ['B', 'A', 'F', 'D'][i % 4]))
        lines.append('')
        t += 1500
    # Auto refocus + monitor + LiveView notes (hunting index = triggers per LV minute)
    lines.append('# monitor open why=keep e=0 t=%d' % (t + 100))
    lines.append('# lv on t=%d' % (t + 1000))
    trig_t = t + 5000
    for k in range(3):
        pct = 96 - k
        lines.append('# aref trig cur=%d ref=%d pct=%d t=%d mm=%d settled_ms=1200'
                     % (pct, 100, pct, trig_t, SCENES[k]['focal']))
        lines.append('# aref done pct=%d land_s=%d new_ref=%d ms=%d path=local mm=%d fine=20 post_close2_s=%d'
                     % (pct, int(_s_at(SCENES[k], SCENES[k]['peak']) * pct / 100), int(_s_at(SCENES[k], SCENES[k]['peak'])),
                        2600 + 400 * k, SCENES[k]['focal'], int(_s_at(SCENES[k], SCENES[k]['peak']) * pct / 100)))
        trig_t += 40000
    lines.append('# aref hold scene in focus new_ref=95000 prev=97000 pre=96000 pen=1 pause_ms=1000 t=%d' % (trig_t + 1000))
    lines.append('# aref skip same failed scene luma=110 g=25000 s=21000 fail_luma=1 fail_g=0 fail_s=0 t=%d' % (trig_t + 2000))
    lv_end = trig_t + 8000
    lines.append('# lv off t=%d' % lv_end)
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    ok_runs = p['nok']
    v = sorted(totals)
    n = len(v)
    med = v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0
    # af_report prints the median with %.0f; the ground truth must be the same number
    med_printed = int('%.0f' % med)
    return dict(runs=runs, ok=ok_runs, median_total_ms=med_printed, totals_ms=totals,
                success_pct=float('%.0f' % (100.0 * ok_runs / runs)), lv_span_ms=lv_end - (t + 1000))


def _write_afbench(path, rng):
    defs = (150, -150, 400, -400, 800, -800)
    # algo, mean land %, mean ms, floor for land % (B and F are the reliable ones on the bench)
    algos = (('A', 94.0, 4700, None), ('B', 97.0, 3500, 91), ('D', 82.0, 5200, None), ('F', 95.0, 3900, 91))
    lines = ['# AFBENCH t=2000 lens="EF-M 18-55mm F3.5-5.6 IS STM" zoom=x5 crop_preset=0 frame_ms=42 '
             'meas_us=4200 log_cap=%d algos=f defocus=150,-150,400,-400,800,-800 def_back=120 reland_pct=70 ok_pct=90'
             % (256 * 1024), '# focal_mm=18 fine=20 fine_setting=2',
             '# ms phase cmd pos s g luma done_val flags done_ms ret dist tries wait samp q']
    summary = {}
    for algo, base_pct, base_ms, floor in algos:
        n = ok = 0
        for d in defs:
            pct = int(round(rng.gauss(base_pct, 5.0)))
            if floor is not None:
                pct = max(floor, pct)
            pct = max(1, min(100, pct))
            ms = int(max(500, round(rng.gauss(base_ms + abs(d) * 1.5, 500.0))))
            ok += pct >= 90
            n += 1
            lines.append('# BENCH algo=%s defocus=%d land_s=%d ref_s=100000 pct=%d moves=8 ms=%d travel=900 ab=0 drift=0'
                         % (algo, d, 100000 * pct // 100, pct, ms))
        summary[algo] = dict(n=n, ok=ok, median_ms=base_ms)
    for algo, d in summary.items():
        lines.append('# BENCH_SUMMARY algo=%s n=%d median_ms=%d ok=%d mean_pct=93'
                     % (algo, d['n'], d['median_ms'], d['ok']))
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return summary


def _write_afrtest(path, rng, profile):
    ok, n, false_trig = PROFILES[profile]['rtest']
    defs = (60, 150, -60, -150, 400)
    lines = ['# RTEST_BEGIN t=5000 lens="EF-M 18-55mm F3.5-5.6 IS STM" zoom=x5 crop_preset=0 mm=18 fine=20 '
             'sens=1 refocus_af=local min_g=26000 settled_ms=1200 settled_pct=92 fail_ms=9000 dwell_ms=1500 '
             'drop_pct=70 peak_min=80000 hold1_ms=5000 hold2_ms=5000 trig_ms=3000 watch_ms=1800 ok_pct=90',
             '# RT_PHASE reference', '# RT_REF why=start t=6000 land_s=97000 ref_s=97000 g=33000 luma=110',
             '# RT_PHASE hold1', '# RT_HOLD1 false_trig=0']
    for k in range(n):
        d = defs[k % len(defs)]
        s_def_pct = 100 - min(60, abs(d) // 4)
        trial_ok = 1 if k < ok else 0
        final_pct = 96 if trial_ok else 78
        lines += ['# RT_PHASE trial def=%d s_def=%d s_def_pct=%d ref_s=97000' % (d, 97000 * s_def_pct // 100, s_def_pct),
                  '# RT_TRIG def=%d run=1 t_ms=800 cur=%d ref=97000 pct=%d path=1 q=4 g=30000 luma=110 settled_ms=1200'
                  % (d, 70000, s_def_pct),
                  '# RT_RUN def=%d run=1 pct=%d ms=%d land_s=%d moves=3 path=local nopeak=0 close2_s=%d'
                  % (d, final_pct, int(rng.gauss(3100, 400)), 94000, 93000),
                  '# RT_TRIAL def=%d s_def=%d s_def_pct=%d trig_ms=800 runs=1 run_ms=3100 eng_pct=%d land_s=94000 '
                  'moves=3 final_s=%d final_pct=%d settled=1 ok=%d ref_s=97000'
                  % (d, 97000 * s_def_pct // 100, s_def_pct, final_pct, 97000 * final_pct // 100, final_pct, trial_ok)]
    lines += ['# RT_PHASE hold2', '# RT_HOLD2 false_trig=%d' % false_trig,
              '# RTEST false_trig=%d trig_ms_med=800 ok=%d/%d final_pct_med=96 runs_total=%d'
              % (false_trig, ok, n, n),
              '# RTEST_END aborted=0 pre_trig=0 trials=%d total_ms=%d' % (n, 120000 + 8000 * n), '']
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def _write_afsweep(path, rng, t0=20000, frame=42.0, lag=42.0, T=1500.0):
    """The shipped crop_rec.c AFSWEEP.LOG format: one chunk per speed/direction,
    samples `S <t_ms> <s1> <g> <luma>` and no t= on the `# sweep` line.  The
    time->position mapping used by af_replay is pos = cmd*(t-t_start-lag)/T."""
    out = ['# AFSWEEP t=%d lens="EF-M 18-55mm F3.5-5.6 IS STM" zoom=x5 crop_preset=0 frame_ms=%d meas_ms=2'
           % (t0 - 100, int(frame)),
           '# meas_us=4200 meas_us_full=8000 meas_stride=2 s4_step=2 done_timeout=2500 extra_frames=2 poll_ms=20 '
           'chunk_07=100 chunk_fast=300 task_prio=1f']
    truth = []
    for k, (cmd, peak) in enumerate(((600, 300.0), (-600, -250.0))):
        t_start = t0 + frame
        out.append('# sweep k=%d cmd=%d speed=7 dir=%d' % (k + 1, abs(cmd), 1 if cmd > 0 else -1))
        t = t_start
        nsamp = 0
        while t <= t_start + T:
            x = cmd * (t - t_start - lag) / T
            luma = int(round(112 + rng.gauss(0.0, 9)))
            s = int(round(B_S * (1.0 + 1.5 * math.exp(-(x - peak) ** 2 / (2.0 * 25.0 ** 2))) * (1.0 + rng.gauss(0, MOVE_NOISE))))
            g = int(round(B_G * (1.0 + 1.2 * math.exp(-(x - peak) ** 2 / (2.0 * 100.0 ** 2))) * (1.0 + rng.gauss(0, MOVE_NOISE))))
            out.append('S %d %d %d %d' % (t, s, g, luma))
            t += frame
            nsamp += 1
        out.append('# done k=%d t=%d val=%s n=%d sent=%d' % (k + 1, int(t_start + T), DONE_SLOW, nsamp, nsamp))
        truth.append(dict(cmd=cmd, peak=peak, sigma_s=25.0, t_start=t_start, t_done=t_start + T, samples=nsamp))
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return truth


def _write_afsearch(path, rng):
    lines = ['# AFSEARCH t=9000 lens="EF-M 18-55mm F3.5-5.6 IS STM" zoom=x5 crop_preset=0',
             '# ms s g g2 luma']
    s = 21000.0
    for i in range(30):
        s *= 1.0 + rng.gauss(0, 0.012)
        g = 26000.0 * (1.0 + rng.gauss(0, 0.012))
        lines.append('%d %d %d %d %d' % (9000 + 42 * i, int(s), int(g), int(g * 0.8), 112 + i % 3))
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def generate(outdir, seed=20261019, profile='healthy'):
    """Write the synthetic session into `outdir` and return its ground truth (also saved
    as outdir/expected.json)."""
    if profile not in PROFILES:
        raise ValueError('unknown profile %r (have %s)' % (profile, ', '.join(sorted(PROFILES))))
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(seed)
    os.makedirs(outdir, exist_ok=True)
    drive = _write_afdrive(os.path.join(outdir, 'AFDRIVE.LOG'), rng, profile)
    bench = _write_afbench(os.path.join(outdir, 'AFBENCH.LOG'), rng)
    _write_afrtest(os.path.join(outdir, 'AFRTEST.LOG'), rng, profile)
    sweeps = _write_afsweep(os.path.join(outdir, 'AFSWEEP.LOG'), rng)
    _write_afsearch(os.path.join(outdir, 'AFSEARCH.LOG'), rng)

    ok, ntrial, false_trig = PROFILES[profile]['rtest']
    checks = {
        'healthy': dict(
            must_contain=['AF drive runs: %d, success %d (100%%)' % (drive['runs'], drive['ok']),
                          'AF drive OK', 'Refocus test (AFRTEST): 1 summary',
                          'Auto refocus trigger rate OK', '== Bench'],
            must_not_contain=['AF drive NOT OK', 'HUNTING', 'CRASH', 'Refocus test: only']),
        'slow': dict(
            must_contain=['AF drive NOT OK', 'Refocus test (AFRTEST): 1 summary'],
            must_not_contain=['AF drive OK (', 'HUNTING', 'CRASH', 'Refocus test: only']),
        'hunting': dict(
            must_contain=['HUNTING', 'Refocus test: only %d/%d ok' % (ok, ntrial), 'AF drive OK'],
            must_not_contain=['AF drive NOT OK', 'CRASH']),
    }[profile]
    meta = {
        'generator': 'contrib/af_test/synth_logs.py',
        'seed': seed,
        'profile': profile,
        'generated': datetime.datetime.now().isoformat(timespec='seconds'),
        'files': sorted(AF_LOGS),
        'scenes': [dict(d) for d in SCENES],
        'afsweep': sweeps,
        'afdrive': drive,
        'bench': bench,
        'rtest': dict(ok=ok, of=ntrial, false_trig=false_trig),
        'checks': dict(
            verdicts=dict(must_contain=checks['must_contain'], must_not_contain=checks['must_not_contain']),
            afdrive=dict(min_runs=drive['runs'], min_success_pct=drive['success_pct'],
                         max_median_ms=drive['median_total_ms']),
            rtest=dict(ok_at_least=ok, of=ntrial, false_trig_max=false_trig),
            bench=dict(algo='B', min_ok=bench['B']['ok'], of=bench['B']['n']),
            replay=dict(min_curve_fits=8),
        ),
    }
    # the file doubles as a ready-made corpus expected.json: the checks are repeated at
    # the top level (the generator's own tests read the same dicts from meta['checks'])
    expected = dict(meta)
    expected.update(meta['checks'])
    with open(os.path.join(outdir, 'expected.json'), 'w') as f:
        json.dump(expected, f, indent=2)
        f.write('\n')
    return meta


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', required=True, help='output directory (created)')
    ap.add_argument('--seed', type=int, default=20261019, help='RNG seed (default 20261019)')
    ap.add_argument('--profile', default='healthy', choices=sorted(PROFILES), help='log profile')
    args = ap.parse_args(argv)
    meta = generate(args.out, seed=args.seed, profile=args.profile)
    print('wrote %d synthetic AF logs to %s (profile %s, seed %d)'
          % (len(meta['files']), args.out, args.profile, args.seed))
    print('ground truth: scenes %s; afdrive %d/%d runs; rtest %d/%d ok, false_trig %d'
          % (', '.join(s['name'] for s in meta['scenes']), meta['afdrive']['ok'], meta['afdrive']['runs'],
             meta['rtest']['ok'], meta['rtest']['of'], meta['rtest']['false_trig']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
