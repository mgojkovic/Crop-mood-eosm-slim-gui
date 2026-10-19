#!/usr/bin/env python3
"""Field-log regression corpus: replay real AF sessions as offline regression tests.

The suite never needs a camera, but a *real* field session is the ground truth that
matters.  This module defines the corpus convention and the replay checks:

  contrib/af_test/corpus/<case-id>/
      AFDRIVE.LOG  AFRTEST.LOG  AFBENCH.LOG  AFSWEEP.LOG ...     <- copied card logs
      expected.json                                             <- the verdicts/limits

`expected.json` (all keys optional, at least one check should be present):

  {
    "name": "2026-10-19 18mm refocus, 35mm bench",
    "source": "/Volumes/EOS_DIGITAL",          // where it was copied from (traceability)
    "notes": "cold session, window light; rtest ok was 3/5 before the local-search fix",
    "verdicts": {                              // substrings of af_report.py's "== Verdict" lines
      "must_contain": ["Refocus test: only 5/5 ok"],   // exact report wording
      "must_not_contain": ["HUNTING", "CRASH"]
    },
    "afdrive": {                               // summary of "== AF drive runs"
      "min_runs": 6, "min_success_pct": 90, "max_median_ms": 5000
    },
    "rtest": {                                 // last "# RTEST false_trig=... ok=k/n" line
      "ok_at_least": 5, "of": 5, "false_trig_max": 1
    },
    "bench": {                                 // "== Bench" summary row for one algorithm
      "algo": "B", "min_ok": 5, "of": 6
    },
    "replay": {                                // af_replay log parse + curve fits
      "min_curve_fits": 6
    }
  }

A case is replayed by running `contrib/af_report/af_report.py --logs <case> --all
--no-state`, checking the text, and (if `replay` is present) by loading the logs with
`contrib/af_replay/af_replay.py` and fitting the focus curves.  A case with no
*.LOG files is skipped, so an empty corpus directory never fails the suite.

Add a case after a field session with:

  python3 contrib/af_test/add_case.py --logs /Volumes/EOS_DIGITAL --name 2026-10-20-refocus

which copies the interesting logs and writes a *baseline* expected.json from the
current report; review and tighten it before committing (see corpus/README.md).

Python 3 standard library only.
"""
import glob
import hashlib
import importlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CORPUS_DIR = os.path.join(HERE, 'corpus')
AF_REPORT = os.path.join(REPO, 'contrib', 'af_report', 'af_report.py')
AF_REPLAY_DIR = os.path.join(REPO, 'contrib', 'af_replay')
AF_REPLAY = os.path.join(AF_REPLAY_DIR, 'af_replay.py')

# logs worth keeping for a case; crash logs are deliberately not copied (af_report --all
# would count every old crash as new)
CORPUS_LOGS = ('AFDRIVE.LOG', 'AFRTEST.LOG', 'AFBENCH.LOG', 'AFSWEEP.LOG', 'AFSTOP.LOG',
               'AFREV.LOG', 'AFCALIB.LOG', 'AFSPEED.LOG', 'AFSEARCH.LOG', 'MPUDRIVE.LOG', 'AFPROBE.LOG')

CHECK_KEYS = ('verdicts', 'afdrive', 'rtest', 'bench', 'replay', 'facts')


# --------------------------------------------------------------------------- discovery

def _looks_like_case(path):
    """A case holds logs and/or an expected.json directly in its directory (a bare README,
    or a session group holding several cases, is not a case)."""
    if os.path.isfile(os.path.join(path, 'expected.json')):
        return True
    return bool(glob.glob(os.path.join(path, '*.LOG')) or glob.glob(os.path.join(path, '*.log')))


def case_dirs(root=None):
    """Every case under the corpus root, sorted; ignores dot dirs.

    Two layouts are supported:
        corpus/<case>/                         (one level)
        corpus/<group>/<case>/                 (a session group, e.g. real-20261019)
    A directory that itself looks like a case is returned; otherwise it is treated as a
    group and its case subdirectories are returned.
    """
    root = root or CORPUS_DIR
    if not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if name.startswith('.') or not os.path.isdir(path):
            continue
        if _looks_like_case(path):
            out.append(path)
            continue
        for sub in sorted(os.listdir(path)):
            child = os.path.join(path, sub)
            if not sub.startswith('.') and os.path.isdir(child) and _looks_like_case(child):
                out.append(child)
    return out


def log_files(case):
    """All *.LOG / *.log files in a case (recursive, so card-style ML/LOGS copies work)."""
    files = []
    for pat in ('*.LOG', '*.log'):
        files += glob.glob(os.path.join(case, '**', pat), recursive=True)
    return sorted(set(files))


def file_fingerprints(case):
    """bytes + sha256 for every log in a case (used to pin the exact fixture content)."""
    out = {}
    for path in log_files(case):
        name = os.path.basename(path)
        with open(path, 'rb') as f:
            data = f.read()
        out[name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    return out


def load_expected(case):
    """Read and validate expected.json.  Raises ValueError with a useful message."""
    path = os.path.join(case, 'expected.json')
    try:
        with open(path) as f:
            exp = json.load(f)
    except OSError:
        raise ValueError('%s: expected.json is missing' % os.path.basename(case))
    except ValueError as e:
        raise ValueError('%s: expected.json is not valid JSON (%s)' % (os.path.basename(case), e))
    if not isinstance(exp, dict):
        raise ValueError('%s: expected.json must be a JSON object' % os.path.basename(case))
    if not any(k in exp for k in CHECK_KEYS):
        raise ValueError('%s: expected.json has no check (%s)'
                         % (os.path.basename(case), ', '.join(CHECK_KEYS)))
    return exp


# --------------------------------------------------------------------------- report parsing

def report_text(case, timeout=120):
    """Run af_report.py on one case, return its stdout (raises RuntimeError on failure)."""
    cmd = [sys.executable, AF_REPORT, '--logs', case, '--all', '--no-state']
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError('af_report.py timed out after %ss on %s' % (timeout, case))
    if p.returncode != 0:
        raise RuntimeError('af_report.py failed (exit %d) on %s:\n%s%s'
                           % (p.returncode, case, p.stdout[-2000:], p.stderr[-2000:]))
    return p.stdout


def parse_metrics(text):
    """Pull the numbers we check out of af_report.py's stdout.  Missing pieces -> absent keys."""
    out = {}
    m = re.search(r'== AF drive runs: (\d+), success (\d+) \(([\d.]+)%\)', text)
    if m:
        out['afdrive'] = dict(runs=int(m.group(1)), ok=int(m.group(2)), success_pct=float(m.group(3)))
        m2 = re.search(r'total_ms med (\d+)', text)
        if m2:
            out['afdrive']['median_total_ms'] = int(m2.group(1))
    m = re.search(r'RTEST false_trig=(-?\d+) trig_ms_med=(-?\d+) ok=(\d+)/(\d+)', text)
    if m:
        out['rtest'] = dict(false_trig=int(m.group(1)), ok=int(m.group(3)), of=int(m.group(4)))
    bench = {}
    for line in text.splitlines():
        m = re.match(r'\s+([A-Z])\s+.*\bn=(\d+) ok=(\d+)\s*$', line)
        if m:
            bench[m.group(1)] = dict(n=int(m.group(2)), ok=int(m.group(3)))
    if bench:
        out['bench'] = bench
    return out


def verdict_lines(text):
    """The '- ...' lines under '== Verdict' (what a field engineer reads)."""
    out, on = [], False
    for line in text.splitlines():
        if line.startswith('== Verdict'):
            on = True
            continue
        if on and line.strip().startswith('- '):
            out.append(line.strip()[2:])
    return out


# --------------------------------------------------------------------------- replay

def _import_af_replay():
    if AF_REPLAY_DIR not in sys.path:
        sys.path.insert(0, AF_REPLAY_DIR)
    return importlib.import_module('af_replay')


def _import_stop_analysis():
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    return importlib.import_module('stop_analysis')


def replay_stats(case):
    """Parse a case's logs with af_replay.py and return count/sweep/fit statistics.

    This is the roll-up the corpus uses to prove the real logs still round-trip through
    the parser and the curve fit; it also carries the per-sweep fits used by sweep facts.
    """
    ar = _import_af_replay()
    data = ar.load_logs(case)
    runs = data['runs']
    sweeps = [sw for _fp, _head, group in data['sweeps'] for sw in group]
    sweep_fits = []
    sweep_series_n = sweep_curve_fits = 0
    for sw in sweeps:
        pos, s, g = ar.sweep_series(sw)
        fit = ar.fit_gauss(pos, s) if len(pos) >= 8 else None
        fit_g = ar.fit_gauss(pos, g) if len(pos) >= 8 else None
        try:
            val = int(sw['done']['val'], 16) if sw.get('done') and sw['done'].get('val') else 0
        except ValueError:
            val = 0
        if len(pos) >= 8:
            sweep_series_n += 1
            if (fit is not None and ar.accept_fit(fit, pos)) or \
               (fit_g is not None and ar.accept_fit(fit_g, pos)):
                sweep_curve_fits += 1
        sweep_fits.append(dict(cmd=sw['cmd'], dir=sw.get('dir'), speed=sw.get('speed'),
                               samples=len(sw['samples']), series=len(pos),
                               timer_done=bool(val & 0x400),
                               sigma_s=round(fit['sig'], 1) if fit else None,
                               peak=round(fit['p'], 1) if fit else None))
    if runs:
        fits = ar.fit_all_series(data)
        series_n = len(fits)
        curve_fits = sum(1 for x in fits if x.get('ok_s') or x.get('ok_g'))
    else:
        series_n, curve_fits = sweep_series_n, sweep_curve_fits
    return dict(
        runs=len(runs),
        moves=sum(len(run['moves']) for run in runs),
        sweeps=len(sweeps),
        sweep_fits=sweep_fits,
        series=series_n,
        curve_fits=curve_fits,
    )


def replay_fits(case):
    """Parse a case's logs with af_replay.py and count usable Gaussian curve fits."""
    stats = replay_stats(case)
    return stats['curve_fits'] if stats else None


RTEST_SUM_RE = re.compile(r'^# RTEST false_trig=(-?\d+) trig_ms_med=(-?\d+) ok=(\d+)/(\d+) '
                          r'final_pct_med=(-?\d+) runs_total=(\d+)')
RT_REF_RE = re.compile(r'^# RT_REF .*?ref_s=(\d+)')
DRIVE_RESULT_RE = re.compile(r'^# RESULT .*?aborted=(\S+)')
BENCH_ROW_RE = re.compile(r'^# BENCH algo=(\S+) defocus=(-?\d+)')
BENCH_SUM_RE = re.compile(r'^# BENCH_SUMMARY algo=(\S+) n=(\d+) median_ms=(-?\d+) ok=(\d+)')


def _case_log(case, name):
    """Path of one named log inside a case (top level first, then recursively)."""
    for path in log_files(case):
        if os.path.basename(path).upper() == name.upper():
            return path
    return None


def _case_lines(case, name):
    path = _case_log(case, name)
    if not path:
        return None
    try:
        with open(path, errors='replace') as f:
            return [line.rstrip('\r\n') for line in f]
    except OSError:
        return None


def _kv_ints(line):
    """Numeric k=v pairs of one log line (ignores string values such as path=local)."""
    out = {}
    for k, v in re.findall(r'(\w+)=(-?\d+)', line):
        out[k] = int(v)
    return out


def parse_rtest_facts(case):
    """Facts from the last Refocus test in AFRTEST.LOG: per-trial final_pct, reference
    sequence (erosion), whether each defocus trial actually triggered, summary counters."""
    lines = _case_lines(case, 'AFRTEST.LOG')
    if not lines:
        return None
    starts = [i for i, line in enumerate(lines) if line.startswith('# RTEST_BEGIN')]
    session = lines[starts[-1]:] if starts else lines
    summaries = [line for line in session if RTEST_SUM_RE.match(line)]
    if not summaries:
        return None
    m = RTEST_SUM_RE.match(summaries[-1])
    summary = dict(false_trig=int(m.group(1)), ok=int(m.group(3)), of=int(m.group(4)),
                   final_pct_med=int(m.group(5)), runs_total=int(m.group(6)))
    trials = []
    for line in session:
        if not line.startswith('# RT_TRIAL '):
            continue
        d = _kv_ints(line)
        if 'def' not in d or 'final_pct' not in d:
            continue
        trials.append(dict(defocus=d['def'], s_def_pct=d.get('s_def_pct'),
                           trig_ms=d.get('trig_ms'), runs=d.get('runs'),
                           run_ms=d.get('run_ms'), eng_pct=d.get('eng_pct'),
                           moves=d.get('moves'), final_pct=d['final_pct'],
                           settled=d.get('settled'), ok=d.get('ok'), ref_s=d.get('ref_s')))
    refs = [int(rt.group(1)) for rt in (RT_REF_RE.match(line) for line in session) if rt]
    refs_monotonic = all(b < a for a, b in zip(refs, refs[1:])) if len(refs) > 1 else None
    return dict(
        summary=summary,
        trials=trials,
        final_pct=[t['final_pct'] for t in trials],
        ref_s=refs,
        ref_s_monotonic_decrease=refs_monotonic,
        triggered_trials=sum(1 for t in trials if (t.get('runs') or 0) > 0),
        never_triggered_def=sorted(t['defocus'] for t in trials if (t.get('runs') or 0) == 0),
    )


def parse_drive_facts(case):
    """Facts from AFDRIVE.LOG: run counters, FOCUS_DONE fast/timer mixture, auto-refocus
    reference drift (`# aref done new_ref=`), and what the local search accepted."""
    lines = _case_lines(case, 'AFDRIVE.LOG')
    if not lines:
        return None
    ar = _import_af_replay()
    path = _case_log(case, 'AFDRIVE.LOG')
    runs = ar.parse_move_log(path) if path else []
    fast = timer = moves = 0
    for run in runs:
        for mv in run['moves']:
            moves += 1
            if mv['done_ms'] <= 0:
                continue
            try:
                val = int(mv['done_val'], 16)
            except ValueError:
                continue
            if val & 0x400:
                timer += 1
            else:
                fast += 1
    results = []
    new_refs = []
    local_in_focus = []
    last_local = None
    for line in lines:
        if line.startswith('# RESULT'):
            m = DRIVE_RESULT_RE.match(line)
            results.append(m.group(1) if m else '?')
        elif line.startswith('# aref done'):
            m = re.search(r'new_ref=(\d+)', line)
            if m:
                new_refs.append(int(m.group(1)))
        elif line.startswith('# local s0='):
            d = _kv_ints(line)
            if 's0' in d and 'last' in d:
                last_local = (d['s0'], d['last'])
        elif line.startswith('# local in focus'):
            d = _kv_ints(line)
            if 's0' in d and 's' in d:
                entry = dict(s0=d['s0'], s=d['s'])
                if last_local:
                    entry['last_landing'] = last_local[1]
                    entry['pct_of_last_s0'] = round(100.0 * d['s0'] / last_local[1], 1)
                    entry['pct_of_last_s'] = round(100.0 * d['s'] / last_local[1], 1)
                local_in_focus.append(entry)
    aborted = {}
    for reason in results:
        aborted[reason] = aborted.get(reason, 0) + 1
    return dict(
        results=len(results),
        aborted=aborted,
        aborted_total=sum(n for reason, n in aborted.items() if reason != '0'),
        fast_moves=fast,
        timer_moves=timer,
        timer_share_pct=round(100.0 * timer / (fast + timer), 1) if (fast + timer) else None,
        moves=moves,
        aref_new_ref=new_refs,
        local_in_focus=local_in_focus,
    )


def parse_bench_facts(case):
    """Facts from AFBENCH.LOG: per-algorithm bench rows, and how much of the requested
    defocus the *positive* half actually produced (s_def vs its reference)."""
    lines = _case_lines(case, 'AFBENCH.LOG')
    if not lines:
        return None
    header = next((line for line in lines if line.startswith('# AFBENCH')), '')
    sym = re.search(r'defocus=([-\d,]+)', header)
    defocus = [int(x) for x in sym.group(1).split(',')] if sym else []
    algo_ref = {}
    trials = []
    rows = {}
    summaries = {}
    for line in lines:
        m = re.match(r'^# BENCH_REF algo=(\S+) ref_s=(\d+)', line)
        if m:
            algo_ref[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r'^# BENCH_TRIAL algo=(\S+) defocus=(-?\d+) s_def=(\d+)', line)
        if m:
            trials.append(dict(algo=m.group(1), defocus=int(m.group(2)), s_def=int(m.group(3)),
                               ref_s=algo_ref.get(m.group(1))))
            continue
        m = BENCH_ROW_RE.match(line)
        if m:
            d = _kv_ints(line)
            rows.setdefault(m.group(1), []).append(d)
            continue
        m = BENCH_SUM_RE.match(line)
        if m:
            summaries[m.group(1)] = dict(n=int(m.group(2)), median_ms=int(m.group(3)), ok=int(m.group(4)))
    algos = {}
    for algo, rs in sorted(rows.items()):
        pcts = [r['pct'] for r in rs if 'pct' in r]
        algos[algo] = dict(
            n=len(rs),
            ok=sum(1 for p in pcts if p >= 90),
            pct=pcts,
            pct_min=min(pcts) if pcts else None,
            pct_max=max(pcts) if pcts else None,
            aborted=sum(1 for r in rs if r.get('ab', '0') not in ('0', 0)),
            summary=summaries.get(algo),
        )
    sdef = {'positive': [], 'negative': []}
    for t in trials:
        if not t['ref_s']:
            continue
        pct = round(100.0 * t['s_def'] / t['ref_s'], 1)
        sdef['positive' if t['defocus'] > 0 else 'negative'].append(pct)
    out = dict(defocus=defocus, algos=algos)
    for key, vals in sdef.items():
        if vals:
            # flat scalars so expected.json can use the same {min,max} range syntax as everything else
            out['s_def_pct_%s_min' % key] = min(vals)
            out['s_def_pct_%s_max' % key] = max(vals)
            out['s_def_pct_%s_values' % key] = sorted(set(vals))
    return out


def parse_sweep_facts(case, stats=None):
    """Facts from AFSWEEP.LOG: one entry per chunk with the fitted s width."""
    stats = stats if stats is not None else replay_stats(case)
    if stats is None or not stats.get('sweeps'):
        return None
    sweeps = stats['sweep_fits']
    sigmas = [s['sigma_s'] for s in sweeps if s.get('sigma_s') is not None]
    return dict(
        sweeps=len(sweeps),
        total_samples=sum(s['samples'] for s in sweeps),
        samples=[s['samples'] for s in sweeps],
        cmd=[s['cmd'] for s in sweeps],
        speed=[s['speed'] for s in sweeps],
        timer_mode_sweeps=sum(1 for s in sweeps if s.get('timer_done')),
        fitted_sweeps=len(sigmas),
        sigma_s_min=min(sigmas) if sigmas else None,
        sigma_s_max=max(sigmas) if sigmas else None,
        sigma_s_median=round(sorted(sigmas)[len(sigmas) // 2], 1) if sigmas else None,
        series=sweeps,
    )


def parse_stop_facts(case):
    """Facts from AFSTOP.LOG: does a `(9,3,1)` stop halt the `(9,1)` move?

    Delegates the per-trial classification to contrib/af_test/stop_analysis.py and
    returns a compact, JSON-friendly roll-up: trial count, planned stop times, the
    halted/continued split, and one row per trial (post-stop delta, least-squares slope
    %/s, max excursion in the move direction, and the halted/continued verdict).
    """
    logs = [p for p in log_files(case) if os.path.basename(p).upper() == 'AFSTOP.LOG']
    if not logs:
        return None
    sa = _import_stop_analysis()
    result = sa.analyze_paths(logs)
    if not result['rows']:
        return None
    rows = []
    for r in result['rows']:
        rows.append(dict(
            card=r['card'], session=r['session'], k=r['k'], at=r['at'], dir=r['dir'],
            t_stop=r['t_stop'], t_done=r['t_done'],
            s_at_stop=r['s_at_stop'], s_end=r['s_end'], delta_pct=r['delta_pct'],
            post_stop_slope_pct_s=r['post_stop_slope_pct_s'],
            max_excursion_pct=r['max_excursion_pct'],
            halted=r['halted'], continued=r['continued']))
    s = result['summary']
    return dict(
        files=s['files'], sessions=s['sessions'], trials=len(rows),
        planned_stop_ms=s['planned_stop_ms'],
        halted_trials=s['halted'], continued_trials=s['continued'],
        changed_trials=s['changed'], incomplete_trials=s['incomplete'],
        median_abs_slope_pct_s=s['median_abs_slope_pct_s'],
        median_abs_delta_pct=s['median_abs_delta_pct'],
        zero_columns=s['zero_columns'],
        rows=rows,
    )


def log_facts(case, stats=None):
    """All raw-log facts available for a case (keys: drive, rtest, bench, sweep, stop)."""
    stats = stats if stats is not None else replay_stats(case)
    facts = {}
    drive = parse_drive_facts(case)
    if drive:
        facts['drive'] = drive
    rtest = parse_rtest_facts(case)
    if rtest:
        facts['rtest'] = rtest
    bench = parse_bench_facts(case)
    if bench:
        facts['bench'] = bench
    sweep = parse_sweep_facts(case, stats)
    if sweep:
        facts['sweep'] = sweep
    stop = parse_stop_facts(case)
    if stop:
        facts['stop'] = stop
    return facts


# --------------------------------------------------------------------------- checks

def check_text(text, expected):
    """Check af_report text against expected.json -> (errors, metrics)."""
    errors = []
    metrics = parse_metrics(text)
    v = expected.get('verdicts') or {}
    verdict = text
    for needle in v.get('must_contain', []):
        if needle not in verdict:
            errors.append('report does not contain %r' % needle)
    for needle in v.get('must_not_contain', []):
        if needle in verdict:
            errors.append('report contains forbidden %r' % needle)
    a = expected.get('afdrive') or {}
    got = metrics.get('afdrive')
    if a and got is None:
        errors.append('expected AF drive runs, but the report has none')
    elif a:
        if 'min_runs' in a and got['runs'] < a['min_runs']:
            errors.append('AF drive runs %d < expected %d' % (got['runs'], a['min_runs']))
        if 'min_success_pct' in a and got['success_pct'] < a['min_success_pct']:
            errors.append('AF drive success %.0f%% < expected %s%%' % (got['success_pct'], a['min_success_pct']))
        if 'max_median_ms' in a:
            med = got.get('median_total_ms')
            if med is None:
                errors.append('expected AF drive median time, but the report has none')
            elif med > a['max_median_ms']:
                errors.append('AF drive median %d ms > expected max %d ms' % (med, a['max_median_ms']))
    r = expected.get('rtest') or {}
    got = metrics.get('rtest')
    if r and got is None:
        errors.append('expected a Refocus test summary, but the report has none')
    elif r:
        if 'ok_at_least' in r and got['ok'] < r['ok_at_least']:
            errors.append('Refocus test ok %d < expected %d' % (got['ok'], r['ok_at_least']))
        if 'of' in r and got['of'] != r['of']:
            errors.append('Refocus test trials %d != expected %d' % (got['of'], r['of']))
        if 'false_trig_max' in r and got['false_trig'] > r['false_trig_max']:
            errors.append('Refocus test false_trig %d > expected max %d'
                          % (got['false_trig'], r['false_trig_max']))
    b = expected.get('bench') or {}
    got = (metrics.get('bench') or {}).get(b.get('algo'))
    if b and got is None:
        errors.append('expected a bench row for algo %r, but the report has none' % b.get('algo'))
    elif b:
        if 'of' in b and got['n'] != b['of']:
            errors.append('bench %s trials %d != expected %d' % (b.get('algo'), got['n'], b['of']))
        if 'min_ok' in b and got['ok'] < b['min_ok']:
            errors.append('bench %s ok %d < expected %d' % (b.get('algo'), got['ok'], b['min_ok']))
    return errors, metrics


def _is_range_spec(spec):
    return isinstance(spec, dict) and any(k in spec for k in ('min', 'max', 'exact', 'approx'))


def _cmp_value(spec, got, path, errors):
    """Compare one expected value/range/list/object tree with what the logs gave."""
    if _is_range_spec(spec):
        if got is None:
            errors.append('%s: expected %r, but the log has no value' % (path, spec))
            return
        if not isinstance(got, (int, float)) or isinstance(got, bool):
            errors.append('%s: expected a number range %r, got %r' % (path, spec, got))
            return
        if 'exact' in spec and abs(got - spec['exact']) > 1e-6:
            errors.append('%s: %r != expected %r' % (path, got, spec['exact']))
        if 'approx' in spec and abs(got - spec['approx']) > spec.get('tol', 0.5):
            errors.append('%s: %r outside expected %r +/- %r'
                          % (path, got, spec['approx'], spec.get('tol', 0.5)))
        if 'min' in spec and got < spec['min']:
            errors.append('%s: %r < expected min %r' % (path, got, spec['min']))
        if 'max' in spec and got > spec['max']:
            errors.append('%s: %r > expected max %r' % (path, got, spec['max']))
        return
    if isinstance(spec, dict):
        if not isinstance(got, dict):
            errors.append('%s: expected an object, got %r' % (path, got))
            return
        for key, sub in spec.items():
            if key in ('note', 'comment', 'meaning'):
                continue
            if key not in got:
                errors.append('%s.%s: missing from the logs' % (path, key))
            else:
                _cmp_value(sub, got[key], '%s.%s' % (path, key), errors)
        return
    if isinstance(spec, list):
        if not isinstance(got, list):
            errors.append('%s: expected a list, got %r' % (path, got))
            return
        if len(spec) != len(got):
            errors.append('%s: got %d entries %r, expected %d %r'
                          % (path, len(got), got[:8], len(spec), spec[:8]))
            return
        for i, (want, have) in enumerate(zip(spec, got)):
            _cmp_value(want, have, '%s[%d]' % (path, i), errors)
        return
    if isinstance(spec, bool) or isinstance(got, bool):
        if bool(spec) != bool(got):
            errors.append('%s: %r != expected %r' % (path, got, spec))
        return
    if isinstance(spec, (int, float)) and isinstance(got, (int, float)):
        if abs(got - spec) > 1e-6:
            errors.append('%s: %r != expected %r' % (path, got, spec))
        return
    if spec != got:
        errors.append('%s: %r != expected %r' % (path, got, spec))


def check_facts(case, facts, expected, metrics=None):
    """Check raw-log facts and the file provenance fingerprints; returns error list."""
    errors = []
    # a case that carries an AFSTOP.LOG must yield stop trials, otherwise the halt
    # evidence silently disappeared (truncated/renamed log, parser regression)
    if _case_log(case, 'AFSTOP.LOG') and 'stop' not in facts:
        errors.append('AFSTOP.LOG is present but no stop trials could be parsed from it')
    want = expected.get('facts')
    if want:
        for area, sub in want.items():
            if area not in facts:
                errors.append('facts.%s: no such facts in the logs (have: %s)'
                              % (area, ', '.join(sorted(facts)) or 'none'))
            else:
                _cmp_value(sub, facts[area], 'facts.' + area, errors)
    # consistency: the raw AFRTEST facts and af_report's text must agree
    if 'rtest' in facts and facts['rtest']:
        r = (metrics or {}).get('rtest')
        fp = facts['rtest']
        if r:
            if (r['ok'], r['of'], r['false_trig']) != (fp['summary']['ok'], fp['summary']['of'],
                                                       fp['summary']['false_trig']):
                errors.append('AFRTEST.LOG and af_report disagree: log ok=%d/%d false_trig=%d vs '
                              'report ok=%d/%d false_trig=%d'
                              % (fp['summary']['ok'], fp['summary']['of'], fp['summary']['false_trig'],
                                 r['ok'], r['of'], r['false_trig']))
    # provenance: sha256 of the copied logs must still match the fixture
    prov = (expected.get('provenance') or {}).get('files') or {}
    if prov:
        for name, meta in prov.items():
            path = _case_log(case, name)
            if not path:
                errors.append('provenance: %s is missing from the case' % name)
                continue
            with open(path, 'rb') as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            if isinstance(meta, dict) and meta.get('sha256'):
                if digest != meta['sha256']:
                    errors.append('provenance: %s changed (sha256 %s... != fixture %s...)'
                                  % (name, digest[:12], meta['sha256'][:12]))
    return errors


def run_case(case, expected=None):
    """Replay one corpus case.  -> dict(name, status, errors, metrics) where status is
    'pass', 'fail' or 'skip' (a case directory without logs is skipped)."""
    name = os.path.basename(case)
    if not os.path.isdir(case):
        return dict(name=name, status='fail', errors=['%s is not a directory' % case], metrics={})
    if not log_files(case):
        return dict(name=name, status='skip', errors=['no *.LOG files (nothing to replay)'], metrics={})
    try:
        expected = expected if expected is not None else load_expected(case)
    except ValueError as e:
        return dict(name=name, status='fail', errors=[str(e)], metrics={})
    try:
        text = report_text(case)
    except RuntimeError as e:
        return dict(name=name, status='fail', errors=[str(e)], metrics={})
    errors, metrics = check_text(text, expected)
    # raw-log facts + provenance (the historical fixture content)
    try:
        stats = replay_stats(case)
        facts = log_facts(case, stats)
    except Exception as e:                           # a parser crash is a failure, not a skip
        errors.append('af_replay failed on the logs: %s: %s' % (type(e).__name__, e))
        stats, facts = None, {}
    else:
        errors += check_facts(case, facts, expected, metrics)
    # replay roll-up: the real logs must still parse into the same shape
    rep = expected.get('replay') or {}
    if rep:
        if stats is None:
            pass
        else:
            checks = dict(min_runs=stats['runs'], min_moves=stats['moves'], min_series=stats['series'],
                          min_curve_fits=stats['curve_fits'], min_sweeps=stats['sweeps'],
                          min_sweep_samples=sum(x['samples'] for x in stats['sweep_fits']))
            for key, got in checks.items():
                if key in rep and got < rep[key]:
                    errors.append('af_replay %s = %d, expected >= %d' % (key, got, rep[key]))
    return dict(name=name, status='fail' if errors else 'pass', errors=errors,
                metrics=metrics, facts=facts)


def run_all_cases(root=None):
    """Replay every case; returns a summary dict with per-case results."""
    results = [run_case(c) for c in case_dirs(root)]
    summary = dict(cases=results,
                   n=len(results),
                   passed=sum(r['status'] == 'pass' for r in results),
                   failed=sum(r['status'] == 'fail' for r in results),
                   skipped=sum(r['status'] == 'skip' for r in results))
    return summary


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description='Replay the AF field-log regression corpus')
    ap.add_argument('--root', default=CORPUS_DIR, help='corpus root (default contrib/af_test/corpus)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)
    summary = run_all_cases(args.root)
    for r in summary['cases']:
        print('%-4s %s%s' % (r['status'].upper(), r['name'],
                             ('  ' + '; '.join(r['errors'])) if r['errors'] and args.verbose else ''))
        if r['errors'] and not args.verbose:
            for e in r['errors']:
                print('      ' + e)
    print('corpus: %d cases, %d pass, %d fail, %d skip' %
          (summary['n'], summary['passed'], summary['failed'], summary['skipped']))
    if not summary['n']:
        print('corpus is empty; add a case with: python3 contrib/af_test/add_case.py '
              '--logs <card-copy> --name <case-id>')
        return 0
    return 1 if summary['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
